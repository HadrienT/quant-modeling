#include <gtest/gtest.h>

#include "quantModeling/market/dupire_from_svi.hpp"
#include "quantModeling/market/superbucket.hpp"
#include "quantModeling/market/svi.hpp"
#include "quantModeling/market/svi_calibration.hpp"
#include "quantModeling/market/svi_surface.hpp"

#include <cmath>
#include <vector>

namespace quantModeling
{
    namespace
    {
        constexpr Real kSpot = 100.0, kRate = 0.02, kDiv = 0.01;
        constexpr Real kKmin = -0.3, kKmax = 0.3;
        constexpr std::size_t kNK = 30, kNT = 12;

        const std::vector<Real> kTtms{0.25, 0.5, 1.0};

        /// A skewed surface: total variance growing with T, put skew.
        SVIParams truth(Real T)
        {
            return SVIParams{0.03 * T, 0.12 * std::sqrt(T), -0.6, 0.02, 0.15};
        }

        std::vector<std::vector<SVISliceQuote>> market_quotes(Real bump_all = 0.0)
        {
            std::vector<std::vector<SVISliceQuote>> out;
            for (const Real T : kTtms)
            {
                std::vector<SVISliceQuote> q;
                for (int i = 0; i <= 14; ++i)
                {
                    const Real k = kKmin + (kKmax - kKmin) * i / 14.0;
                    q.push_back({k, svi_implied_vol(k, T, truth(T)) + bump_all, 1.0});
                }
                out.push_back(q);
            }
            return out;
        }

        std::vector<SVISliceCalibration> calibrate(const std::vector<std::vector<SVISliceQuote>> &q)
        {
            std::vector<SVISliceCalibration> out;
            for (std::size_t s = 0; s < kTtms.size(); ++s)
                out.push_back(calibrate_svi_slice(q[s], kTtms[s]));
            return out;
        }

        /// The "price": a fixed linear functional of the local-vol grid.
        std::vector<Real> weights()
        {
            std::vector<Real> c(kNK * kNT);
            for (std::size_t i = 0; i < c.size(); ++i)
                c[i] = std::sin(0.37 * static_cast<Real>(i)) + 0.5;
            return c;
        }

        Real price(const std::vector<SVISliceCalibration> &slices)
        {
            const SVISurface surface(slices);
            const GridLocalVol grid =
                build_local_vol_grid(surface, kSpot, kRate, kDiv, kKmin, kKmax, kNK, kNT);
            const std::vector<Real> c = weights();
            Real v = 0.0;
            for (std::size_t i = 0; i < c.size(); ++i)
                v += c[i] * grid.sigma_loc()[i];
            return v;
        }

        SuperbucketResult bucket(const std::vector<SVISliceCalibration> &slices,
                                 const std::vector<std::vector<SVISliceQuote>> &q)
        {
            return dupire_superbucket(slices, q, kSpot, kRate, kDiv, kKmin, kKmax, kNK, kNT, weights());
        }
    } // namespace

    TEST(Superbucket, ThroughDupireTheTapeMatchesBumpingEachSVIParameter)
    {
        const auto q = market_quotes();
        const auto slices = calibrate(q);
        const SuperbucketResult r = bucket(slices, q);

        for (std::size_t s = 0; s < slices.size(); ++s)
            for (std::size_t j = 0; j < 5; ++j)
            {
                const Real h = 1e-6;
                auto up = slices, dn = slices;
                std::vector<Real> pu = SVISliceObjective::pack(up[s].params),
                                  pd = SVISliceObjective::pack(dn[s].params);
                pu[j] += h;
                pd[j] -= h;
                up[s].params = SVISliceObjective::unpack(pu);
                dn[s].params = SVISliceObjective::unpack(pd);
                const Real fd = (price(up) - price(dn)) / (2 * h);
                EXPECT_NEAR(r.dV_dsvi[s][j], fd, 1e-5 * (1.0 + std::fabs(fd)))
                    << "slice " << s << " param " << j;
            }
    }

    TEST(Superbucket, AQuoteVegaIsTheBumpRecalibrateReprice)
    {
        // The desk's definition of the number: move one quoted vol, refit its
        // slice, rebuild the local vol, reprice. The superbucket gets it in
        // one pass for every quote.
        const auto q = market_quotes();
        const auto slices = calibrate(q);
        const SuperbucketResult r = bucket(slices, q);

        std::size_t checked = 0;
        for (const QuoteVega &v : r.quotes)
        {
            if (v.slice != 1 || (checked++ % 3) != 0)
                continue; // a sample of the middle slice's quotes
            const std::size_t i = static_cast<std::size_t>(
                std::lround((v.log_moneyness - kKmin) / (kKmax - kKmin) * 14.0));
            const Real h = 1e-4;
            auto qu = q, qd = q;
            qu[1][i].market_iv += h;
            qd[1][i].market_iv -= h;
            auto su = slices, sd = slices;
            su[1] = calibrate_svi_slice(qu[1], kTtms[1]);
            sd[1] = calibrate_svi_slice(qd[1], kTtms[1]);
            const Real fd = (price(su) - price(sd)) / (2 * h);
            EXPECT_NEAR(v.vega, fd, 0.02 * std::fabs(fd) + 1e-3) << "k = " << v.log_moneyness;
        }
        EXPECT_GE(checked, 10u);
    }

    TEST(Superbucket, TheQuoteVegasAddUpToAParallelShiftOfTheWholeSurface)
    {
        const auto q = market_quotes();
        const SuperbucketResult r = bucket(calibrate(q), q);
        Real total = 0.0;
        for (const QuoteVega &v : r.quotes)
            total += v.vega;

        const Real h = 1e-4;
        const Real fd = (price(calibrate(market_quotes(h))) - price(calibrate(market_quotes(-h)))) / (2 * h);
        EXPECT_NEAR(total, fd, 0.02 * std::fabs(fd));
        EXPECT_GT(std::fabs(total), 1.0); // a real sensitivity, not a vacuous zero
    }

    TEST(Superbucket, RejectsMisshapenInputs)
    {
        const auto q = market_quotes();
        const auto slices = calibrate(q);
        EXPECT_THROW(dupire_superbucket(slices, {q[0]}, kSpot, kRate, kDiv, kKmin, kKmax, kNK, kNT, weights()),
                     InvalidInput);
        EXPECT_THROW(dupire_superbucket(slices, q, kSpot, kRate, kDiv, kKmin, kKmax, kNK, kNT, {1.0}),
                     InvalidInput);
    }

} // namespace quantModeling
