// The superbucket through the eSSVI calibration (#381, blueprint/wp/30-vol-surface.md,
// lot E3). The desk's definition of a quote's vega -- move one quoted vol,
// recalibrate the whole surface, rebuild the local vol, reprice -- checked
// against the one-pass adjoint for quotes that reach the price by every road:
// a wing quote through its slice's fit only, an at-the-money quote through the
// term structure too, quotes of a slice whose calendar bound binds (so its
// risk flows into the slice before), and quotes of a slice left out of the
// surface (they still move the term structure).

#include <gtest/gtest.h>

#include "quantModeling/market/dupire_from_svi.hpp"
#include "quantModeling/market/essvi.hpp"
#include "quantModeling/market/superbucket.hpp"

#include <cmath>
#include <random>
#include <vector>

namespace quantModeling
{
    namespace
    {
        constexpr Real kSpot = 100.0, kRate = 0.02, kDiv = 0.01, kKmin = -0.5, kKmax = 0.35;
        constexpr std::size_t kNK = 36, kNT = 24;
        const std::vector<Real> kT = {0.08, 0.25, 0.5, 0.75, 1.0, 1.5};

        /// An equity-like surface, then noise and two contradictions: the
        /// fourth slice quoted low at the money (its forward variance would
        /// fall), the last with a steeper skew than calendar allows.
        /// The second scenario: the 1y slice's skew 30 % steeper than the
        /// slices around it (its phi would rise: the calendar bound psi <=
        /// psi_before theta / theta_before binds), a forward-vol floor above
        /// the short end's forward vol (forward variances at their floor), and
        /// the 0.5y slice quoted on four strikes only (left out of the surface
        /// and of the term structure: its vegas are zero).
        bool g_binding = false;

        ESSVICalibrationSettings settings()
        {
            ESSVICalibrationSettings st;
            if (g_binding)
                st.min_forward_vol = 0.205;
            return st;
        }

        std::vector<std::vector<SVISliceQuote>> market(Real bump_all = 0.0)
        {
            std::mt19937 rng(11);
            std::normal_distribution<Real> noise(0.0, 0.003);
            std::vector<std::vector<SVISliceQuote>> out;
            for (std::size_t s = 0; s < kT.size(); ++s)
            {
                const Real t = kT[s], theta = 0.04 * t * (1.0 + 0.1 * t);
                const Real steep = g_binding && s == 4 ? 1.3 : 1.0;
                const ESSVIParams p{theta, -0.7 + 0.1 * t / (1.0 + t), steep * 1.1 * std::sqrt(theta)};
                std::vector<SVISliceQuote> q;
                const Real width = 0.7 * std::sqrt(std::min(t, Real(2.0)) + 0.05);
                for (int i = 0; i <= 24; ++i)
                {
                    const Real k = -width + 1.4 * width * i / 24.0;
                    Real iv = std::sqrt(essvi_total_variance(k, p) / t) + noise(rng);
                    if (!g_binding && s == 3)
                        iv -= 0.012 * std::exp(-k * k / 0.02);
                    if (!g_binding && s == 5)
                        iv += 0.03 * std::tanh(-4.0 * k);
                    if (g_binding && s == 2 && i % 6 != 0)
                        continue;
                    q.push_back({k, iv + bump_all, 1.0});
                }
                out.push_back(q);
            }
            return out;
        }

        std::vector<Real> weights()
        {
            std::vector<Real> c(kNK * kNT);
            for (std::size_t i = 0; i < c.size(); ++i)
                c[i] = std::sin(0.37 * static_cast<Real>(i)) + 0.5;
            return c;
        }

        Real price_of(const std::vector<ESSVISliceFit> &fits)
        {
            // The surface calibrate_vol_surface builds: its pillars only.
            const std::vector<bool> pillar = essvi_surface_pillars(fits, settings().surface_min_relative_gap);
            std::vector<ESSVISliceFit> pillars;
            for (std::size_t i = 0; i < fits.size(); ++i)
                if (fits[i].feasible && pillar[i])
                    pillars.push_back(fits[i]);
            const ESSVISurface s(pillars);
            const GridLocalVol g = build_local_vol_grid(s, kSpot, kRate, kDiv, kKmin, kKmax, kNK, kNT);
            const std::vector<Real> c = weights();
            Real v = 0.0;
            for (std::size_t i = 0; i < c.size(); ++i)
                v += c[i] * g.sigma_loc()[i];
            return v;
        }

        Real price(const std::vector<std::vector<SVISliceQuote>> &q)
        {
            return price_of(calibrate_essvi_slices(q, kT, settings()));
        }

        SuperbucketResult bucket(const std::vector<std::vector<SVISliceQuote>> &q)
        {
            return essvi_superbucket(q, kT, settings(), kSpot, kRate, kDiv, kKmin, kKmax, kNK, kNT, weights());
        }

        /// The quote vegas against bump-recalibrate-reprice on every slice's
        /// at-the-money quote and both wings; `rel` the tolerance relative
        /// to the bump's value.
        void check_quote_vegas(Real rel)
        {
            const auto q = market();
            const SuperbucketResult r = bucket(q);
            Real scale = 0.0;
            for (const QuoteVega &v : r.quotes)
                scale = std::max(scale, std::fabs(v.vega));
            ASSERT_GT(scale, 0.1);
            std::size_t row = 0, checked = 0;
            for (std::size_t s = 0; s < q.size(); ++s)
                for (std::size_t i = 0; i < q[s].size(); ++i, ++row)
                {
                    const std::size_t mid = q[s].size() / 2;
                    if (i != mid && i != q[s].size() / 8 && i != q[s].size() - 1 - q[s].size() / 8)
                        continue;
                    const Real h = 1e-4;
                    auto qu = q, qd = q;
                    qu[s][i].market_iv += h;
                    qd[s][i].market_iv -= h;
                    const Real fd = (price(qu) - price(qd)) / (2 * h);
                    EXPECT_NEAR(r.quotes[row].vega, fd, rel * std::fabs(fd) + 1e-5 * scale)
                        << "slice " << s << ", quote " << i << ", k " << q[s][i].log_moneyness;
                    ++checked;
                }
            EXPECT_GE(checked, 2 * q.size());
        }
    } // namespace

    TEST(ESSVISuperbucket, ThroughDupireTheTapeMatchesBumpingEachParameter)
    {
        const auto q = market();
        const std::vector<ESSVISliceFit> fits = calibrate_essvi_slices(q, kT);
        const SuperbucketResult r = bucket(q);
        for (std::size_t s = 0; s < fits.size(); ++s)
        {
            if (!fits[s].feasible)
                continue;
            for (int j = 0; j < 3; ++j)
            {
                const Real h = 1e-7;
                auto up = fits, dn = fits;
                Real *pu = j == 0 ? &up[s].params.theta : j == 1 ? &up[s].params.rho
                                                                 : &up[s].params.psi;
                Real *pd = j == 0 ? &dn[s].params.theta : j == 1 ? &dn[s].params.rho
                                                                 : &dn[s].params.psi;
                *pu += h, *pd -= h;
                const Real fd = (price_of(up) - price_of(dn)) / (2 * h);
                EXPECT_NEAR(r.dV_dessvi[s][static_cast<std::size_t>(j)], fd, 1e-5 * (1.0 + std::fabs(fd)))
                    << "slice " << s << " param " << j;
            }
        }
    }

    TEST(ESSVISuperbucket, AQuoteVegaIsTheBumpRecalibrateReprice)
    {
        // Noise and contradicting slices. The implicit function theorem is
        // exact (full Hessians, constraint curvature), so the two agree to the
        // bump's own noise, ~1e-4.
        g_binding = false;
        check_quote_vegas(1e-3);
    }

    TEST(ESSVISuperbucket, AQuoteVegaIsTheBumpRecalibrateRepriceWhenBoundsBind)
    {
        struct Reset
        {
            ~Reset() { g_binding = false; }
        } reset;
        g_binding = true;
        const auto q = market();
        const std::vector<ESSVISliceFit> fits = calibrate_essvi_slices(q, kT, settings());
        // the scenario does what it says
        ASSERT_FALSE(fits[2].feasible);
        ASSERT_TRUE(fits[4].feasible && fits[3].feasible);
        EXPECT_NEAR(fits[4].params.psi, fits[3].params.psi * fits[4].params.theta / fits[3].params.theta,
                    1e-7 * fits[4].params.psi);
        int at_floor = 0; // forward variances at the floor, between feasible slices
        Real before_theta = 0.0, before_T = 0.0;
        for (const ESSVISliceFit &f : fits)
            if (f.feasible)
            {
                at_floor += std::fabs((f.params.theta - before_theta) / (f.ttm - before_T) - 0.205 * 0.205) < 1e-12;
                before_theta = f.params.theta, before_T = f.ttm;
            }
        EXPECT_GE(at_floor, 1);
        const SuperbucketResult r = bucket(q);
        for (const QuoteVega &v : r.quotes)
            if (v.slice == 2)
                EXPECT_EQ(v.vega, 0.0);
        check_quote_vegas(1e-3);
    }

    TEST(ESSVISuperbucket, TheQuoteVegasAddUpToAParallelShiftOfTheWholeSurface)
    {
        const SuperbucketResult r = bucket(market());
        Real total = 0.0;
        for (const QuoteVega &v : r.quotes)
            total += v.vega;
        const Real h = 1e-4;
        const Real fd = (price(market(h)) - price(market(-h))) / (2 * h);
        EXPECT_NEAR(total, fd, 1e-3 * std::fabs(fd));
        EXPECT_GT(std::fabs(total), 1.0);
    }

    TEST(ESSVISuperbucket, RejectsMisshapenInputs)
    {
        const auto q = market();
        EXPECT_THROW(essvi_superbucket({q[0]}, kT, {}, kSpot, kRate, kDiv, kKmin, kKmax, kNK, kNT, weights()),
                     InvalidInput);
        EXPECT_THROW(essvi_superbucket(q, kT, {}, kSpot, kRate, kDiv, kKmin, kKmax, kNK, kNT, {1.0}), InvalidInput);
        ESSVICalibrationSettings free_theta;
        free_theta.smooth_theta = false;
        EXPECT_THROW(essvi_superbucket(q, kT, free_theta, kSpot, kRate, kDiv, kKmin, kKmax, kNK, kNT, weights()),
                     InvalidInput);
    }

} // namespace quantModeling
