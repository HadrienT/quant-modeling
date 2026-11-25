// The extended SSVI surface (#381, blueprint/wp/30-vol-surface.md): free of
// static arbitrage by construction, calibrated slice by slice.
//
// Properties checked:
//  - a slice is the SVI slice essvi_to_svi says, and its analytic
//    derivatives in k and the surface's in T are the finite differences';
//  - noiseless quotes from a known arbitrage-free surface give it back;
//  - quotes with a slice that contradicts its neighbours (SPY's: a long
//    slice whose at-the-money vol breaks the term structure, a short one
//    whose curvature sits on a point) give slices that meet Gatheral &
//    Jacquier's butterfly bounds and Hendriks & Martini's calendar
//    conditions, and between them a surface whose total variance never
//    falls in T, whose density g stays positive, whose local variance is
//    positive and finite at every grid point -- no hole to fill;
//  - its local-vol grid needs no fallback cell.

#include <gtest/gtest.h>

#include "quantModeling/market/dupire_from_svi.hpp"
#include "quantModeling/market/essvi.hpp"
#include "quantModeling/market/svi.hpp"

#include <cmath>
#include <random>
#include <vector>

namespace quantModeling
{
    namespace
    {
        /// An arbitrage-free eSSVI term structure, equity-like: theta and psi
        /// increasing, psi = 1.1 sqrt(theta) inside the butterfly bound
        /// psi^2 (1 + |rho|) <= 4 theta, rho flattening slowly enough for the
        /// calendar condition.
        std::vector<ESSVIParams> reference(const std::vector<Real> &T)
        {
            std::vector<ESSVIParams> out;
            for (const Real t : T)
            {
                const Real theta = 0.04 * t * (1.0 + 0.1 * t);
                const Real psi = 1.1 * std::sqrt(theta);
                const Real rho = -0.7 + 0.1 * t / (1.0 + t);
                out.push_back({theta, rho, psi});
            }
            return out;
        }

        std::vector<SVISliceQuote> quotes_of(const ESSVIParams &p, Real T, Real noise = 0.0, unsigned seed = 1)
        {
            std::mt19937 rng(seed);
            std::normal_distribution<Real> n(0.0, noise);
            std::vector<SVISliceQuote> q;
            const Real width = 0.35 * std::sqrt(std::min(T, Real(2.0)) + 0.05) * 2.0;
            for (int i = 0; i <= 40; ++i)
            {
                const Real k = -width + 1.4 * width * i / 40.0;
                SVISliceQuote x;
                x.log_moneyness = k;
                x.market_iv = std::sqrt(essvi_total_variance(k, p) / T) + (noise > 0.0 ? n(rng) : 0.0);
                x.weight = 1.0;
                q.push_back(x);
            }
            return q;
        }

        void expect_arbitrage_free(const ESSVISurface &s, const char *what)
        {
            for (std::size_t i = 0; i < s.slices().size(); ++i)
            {
                EXPECT_TRUE(essvi_butterfly_free(s.slices()[i].params)) << what << ", slice " << i;
                if (i > 0)
                    EXPECT_TRUE(essvi_calendar_free(s.slices()[i - 1].params, s.slices()[i].params))
                        << what << ", slices " << i - 1 << ", " << i;
            }
            for (Real T = 0.02; T <= 1.3 * s.ttm_max(); T += 0.01)
            {
                const ESSVIParams p = s.params(T);
                EXPECT_TRUE(essvi_butterfly_free(p)) << what << ", T " << T;
                for (Real k = -1.0; k <= 1.0; k += 0.05)
                {
                    EXPECT_GE(s.total_variance_dT(k, T), -1e-12) << what << ", calendar at k " << k << ", T " << T;
                    const Real w = s.total_variance(k, T), wp = s.total_variance_dk(k, T),
                               wpp = s.total_variance_dk2(k, T);
                    EXPECT_GE(svi_density_g_from_variance(k, w, wp, wpp), -1e-12)
                        << what << ", butterfly at k " << k << ", T " << T;
                    const Real lv = svi_surface_local_variance(s, k, T, DupireFromSVIParams{0.0, 0.0, 1e9});
                    EXPECT_TRUE(std::isfinite(lv) && lv > 0.0) << what << ", local variance at k " << k << ", T " << T;
                }
            }
        }
    } // namespace

    TEST(ESSVI, ASliceIsTheSviSliceAndItsDerivativesAreExact)
    {
        for (const ESSVIParams p : {ESSVIParams{0.04, -0.7, 0.3}, ESSVIParams{0.1, 0.3, 0.8}, ESSVIParams{0.01, 0.0, 0.05}})
        {
            const SVIParams s = essvi_to_svi(p);
            EXPECT_NEAR(essvi_total_variance(0.0, p), p.theta, 1e-15);
            for (Real k = -1.0; k <= 1.0; k += 0.1)
            {
                EXPECT_NEAR(essvi_total_variance(k, p), svi_total_variance(k, s), 1e-14);
                const Real h = 1e-5;
                EXPECT_NEAR(essvi_total_variance_dk(k, p),
                            (essvi_total_variance(k + h, p) - essvi_total_variance(k - h, p)) / (2 * h), 1e-8);
                EXPECT_NEAR(essvi_total_variance_dk2(k, p),
                            (essvi_total_variance_dk(k + h, p) - essvi_total_variance_dk(k - h, p)) / (2 * h), 1e-6);
            }
        }
    }

    TEST(ESSVI, NoiselessQuotesGiveTheSurfaceBack)
    {
        const std::vector<Real> T = {0.08, 0.25, 0.5, 1.0, 2.0};
        const std::vector<ESSVIParams> ref = reference(T);
        std::vector<std::vector<SVISliceQuote>> q;
        for (std::size_t i = 0; i < T.size(); ++i)
        {
            ASSERT_TRUE(essvi_butterfly_free(ref[i])) << i;
            if (i > 0)
                ASSERT_TRUE(essvi_calendar_free(ref[i - 1], ref[i])) << i;
            q.push_back(quotes_of(ref[i], T[i]));
        }
        // theta free in each slice: exact
        ESSVICalibrationSettings free_theta;
        free_theta.smooth_theta = false;
        const std::vector<ESSVISliceFit> fits = calibrate_essvi_slices(q, T, free_theta);
        // on the smoothed term structure (the default): within a few tenths
        // of a vol point, the smoothing's own bias on a curved forward variance
        for (const ESSVISliceFit &f : calibrate_essvi_slices(q, T))
        {
            ASSERT_TRUE(f.feasible) << f.ttm;
            EXPECT_LT(f.rmse, 0.004) << f.ttm;
        }
        for (std::size_t i = 0; i < T.size(); ++i)
        {
            ASSERT_TRUE(fits[i].feasible) << i;
            EXPECT_LT(fits[i].rmse, 2e-5) << i;
            EXPECT_NEAR(fits[i].params.theta, ref[i].theta, 1e-4 * ref[i].theta) << i;
            EXPECT_NEAR(fits[i].params.rho, ref[i].rho, 2e-3) << i;
            EXPECT_NEAR(fits[i].params.psi, ref[i].psi, 2e-3 * ref[i].psi) << i;
        }
        const ESSVISurface s(fits);
        expect_arbitrage_free(s, "noiseless");
        // the surface's T-derivative is the finite difference's within a segment
        for (const Real t : {0.15, 0.7, 1.5, 2.5})
            for (const Real k : {-0.3, 0.0, 0.2})
                EXPECT_NEAR(s.total_variance_dT(k, t),
                            (s.total_variance(k, t + 1e-6) - s.total_variance(k, t - 1e-6)) / 2e-6, 1e-6)
                    << k << " " << t;
    }

    TEST(ESSVI, QuotesThatContradictEachOtherStillGiveAnArbitrageFreeSurface)
    {
        // Noise, a long slice quoted two vol points too high at the money
        // (its neighbours then cannot both hold calendar), and a short slice
        // with a kink -- the shapes that put holes in SPY's local vol (#381).
        const std::vector<Real> T = {0.08, 0.25, 0.6, 0.85, 1.1, 1.56, 2.06};
        const std::vector<ESSVIParams> ref = reference(T);
        std::vector<std::vector<SVISliceQuote>> q;
        for (std::size_t i = 0; i < T.size(); ++i)
            q.push_back(quotes_of(ref[i], T[i], 0.004, 7 + i));
        for (SVISliceQuote &x : q[5])
            x.market_iv += 0.02 * std::exp(-x.log_moneyness * x.log_moneyness / 0.02);
        for (SVISliceQuote &x : q[3])
            x.market_iv -= 0.03 * std::exp(-(x.log_moneyness - 0.05) * (x.log_moneyness - 0.05) / 0.0005);
        const std::vector<ESSVISliceFit> fits = calibrate_essvi_slices(q, T);
        std::size_t kept = 0;
        for (const ESSVISliceFit &f : fits)
            kept += f.feasible;
        EXPECT_GE(kept, T.size() - 1); // the contradicting slice may go, the others stay
        const ESSVISurface s(fits);
        expect_arbitrage_free(s, "contradicting");
        for (const ESSVISliceFit &f : fits)
            if (f.feasible)
                EXPECT_LT(f.rmse, 0.02) << "T " << f.ttm;

        // The grid: every cell computed, none filled from a neighbour.
        const GridLocalVol grid = build_local_vol_grid(s, 100.0, 0.03, 0.01, -0.6, 0.4, 60, 30);
        for (const Real v : grid.sigma_loc())
        {
            EXPECT_GT(v, 0.02);
            EXPECT_LT(v, 1.5);
        }
    }

} // namespace quantModeling
