#include <gtest/gtest.h>

#include "quantModeling/market/dupire_from_svi.hpp"

#include <cmath>
#include <vector>

namespace quantModeling
{
    namespace
    {

        SVISliceCalibration flat_vol_slice(Real ttm, Real sigma)
        {
            SVISliceCalibration s;
            s.ttm = ttm;
            s.params = SVIParams{sigma * sigma * ttm, 0.0, 0.0, 0.0, 0.1}; // b = 0: w(k) = a for every k
            s.butterfly_arbitrage_free = true;
            return s;
        }

        SVISliceCalibration smile_slice(Real ttm, Real a)
        {
            SVISliceCalibration s;
            s.ttm = ttm;
            s.params = SVIParams{a, 0.10, -0.40, 0.0, 0.30};
            s.butterfly_arbitrage_free = true;
            return s;
        }

    } // namespace

    // ── Flat-vol round trip: Dupire must give back the constant Black-Scholes
    //    vol exactly when the "smile" has no strike or maturity dependence. ──

    TEST(DupireFromSVI, RecoversTheConstantVolatilityOfAFlatSurface)
    {
        constexpr Real sigma = 0.20;
        SVISurface surface({flat_vol_slice(0.25, sigma), flat_vol_slice(1.00, sigma)});

        for (const Real T : {0.30, 0.5, 0.75, 0.95})
        {
            for (const Real k : {-0.3, -0.1, 0.0, 0.1, 0.3})
            {
                const Real local_var = svi_surface_local_variance(surface, k, T);
                ASSERT_FALSE(std::isnan(local_var));
                EXPECT_NEAR(local_var, sigma * sigma, 1e-10);
            }
        }
    }

    TEST(DupireFromSVI, BuildsAFullyFlatGridFromAFlatSurface)
    {
        constexpr Real sigma = 0.20, spot = 100.0;
        SVISurface surface({flat_vol_slice(0.25, sigma), flat_vol_slice(1.00, sigma)});

        const GridLocalVol grid = build_local_vol_grid(surface, spot, 0.0, 0.0, -0.5, 0.5, 20, 15);

        for (const Real s : {80.0, 90.0, 100.0, 110.0, 120.0})
            for (const Real t : {0.3, 0.5, 0.8})
                EXPECT_NEAR(grid.value(s, t), sigma, 1e-3);
    }

    // ── Calendar arbitrage is caught, not silently priced through ──────────

    TEST(DupireFromSVI, ReturnsNaNWhereTotalVarianceDecreasesInMaturity)
    {
        SVISurface surface({smile_slice(0.25, 0.08), smile_slice(1.00, 0.02)}); // a decreases: arbitrage
        const Real local_var = svi_surface_local_variance(surface, 0.0, 0.6);
        EXPECT_TRUE(std::isnan(local_var));
    }

    TEST(DupireFromSVI, IsFiniteEverywhereForAGenuinelyIncreasingSmileSurface)
    {
        SVISurface surface({smile_slice(0.25, 0.02), smile_slice(1.00, 0.10)});
        for (const Real T : {0.3, 0.5, 0.8})
            for (const Real k : {-0.3, -0.1, 0.0, 0.1, 0.3})
                EXPECT_FALSE(std::isnan(svi_surface_local_variance(surface, k, T)));
    }

    // ── Grid construction ────────────────────────────────────────────────────

    TEST(DupireFromSVI, GridHasNoResidualNaNsAfterGapFilling)
    {
        SVISurface surface({smile_slice(0.25, 0.02), smile_slice(1.00, 0.10)});
        const GridLocalVol grid = build_local_vol_grid(surface, 100.0, 0.02, 0.0, -0.6, 0.6, 40, 20);

        for (const Real sigma : grid.sigma_loc())
        {
            EXPECT_FALSE(std::isnan(sigma));
            EXPECT_GT(sigma, 0.0);
        }
    }

    TEST(DupireFromSVI, GridValueAtANodeMatchesTheDirectFormulaThere)
    {
        SVISurface surface({smile_slice(0.25, 0.02), smile_slice(1.00, 0.10)});
        const Real spot = 100.0, rate = 0.0, dividend = 0.0;
        const GridLocalVol grid = build_local_vol_grid(surface, spot, rate, dividend, -0.6, 0.6, 30, 12);

        // An interior grid node: value() should reproduce the stored
        // sigma_loc exactly there (no interpolation needed at a node).
        const std::size_t i = grid.K_grid().size() / 2;
        const std::size_t j = grid.T_grid().size() / 2;
        const Real K = grid.K_grid()[i];
        const Real T = grid.T_grid()[j];

        EXPECT_NEAR(grid.value(K, T), grid.sigma_loc()[i * grid.T_grid().size() + j], 1e-9);
    }

    TEST(DupireFromSVI, RespectsTheConfiguredLocalVarianceBounds)
    {
        SVISurface surface({smile_slice(0.25, 0.02), smile_slice(1.00, 0.10)});
        DupireFromSVIParams params;
        params.min_local_var = 0.05 * 0.05;
        params.max_local_var = 0.60 * 0.60;

        const GridLocalVol grid = build_local_vol_grid(surface, 100.0, 0.0, 0.0, -0.6, 0.6, 25, 10, params);
        for (const Real sigma : grid.sigma_loc())
        {
            EXPECT_GE(sigma, 0.05 - 1e-9);
            EXPECT_LE(sigma, 0.60 + 1e-9);
        }
    }

} // namespace quantModeling
