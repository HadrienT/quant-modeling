#include <gtest/gtest.h>

#include "quantModeling/core/types.hpp"
#include "quantModeling/market/svi_surface.hpp"

#include <cmath>
#include <vector>

namespace quantModeling
{
    namespace
    {

        SVISliceCalibration make_slice(Real ttm, Real a)
        {
            SVISliceCalibration s;
            s.ttm = ttm;
            s.params = SVIParams{a, 0.10, -0.40, 0.0, 0.30};
            s.butterfly_arbitrage_free = true;
            return s;
        }

        SVISurface two_slice_surface()
        {
            return SVISurface({make_slice(0.25, 0.02), make_slice(1.00, 0.10)});
        }

    } // namespace

    TEST(SVISurface, RequiresAtLeastTwoSlices)
    {
        EXPECT_THROW(SVISurface({make_slice(0.25, 0.02)}), InvalidInput);
    }

    TEST(SVISurface, RejectsDuplicateMaturities)
    {
        EXPECT_THROW(SVISurface({make_slice(0.5, 0.02), make_slice(0.5, 0.03)}), InvalidInput);
    }

    TEST(SVISurface, SortsSlicesRegardlessOfConstructionOrder)
    {
        SVISurface surface({make_slice(1.00, 0.10), make_slice(0.25, 0.02)});
        EXPECT_DOUBLE_EQ(surface.ttm_min(), 0.25);
        EXPECT_DOUBLE_EQ(surface.ttm_max(), 1.00);
    }

    TEST(SVISurface, MatchesTheSliceExactlyAtItsOwnMaturity)
    {
        const SVISurface surface = two_slice_surface();
        for (const Real k : {-0.3, -0.1, 0.0, 0.1, 0.3})
        {
            EXPECT_NEAR(surface.total_variance(k, 0.25), svi_total_variance(k, surface.slices()[0].params), 1e-12);
            EXPECT_NEAR(surface.total_variance(k, 1.00), svi_total_variance(k, surface.slices()[1].params), 1e-12);
        }
    }

    TEST(SVISurface, IsLinearInMaturityAtFixedLogMoneyness)
    {
        const SVISurface surface = two_slice_surface();
        const Real k = 0.05;
        const Real w0 = surface.total_variance(k, 0.25);
        const Real w1 = surface.total_variance(k, 1.00);
        const Real T_mid = 0.5 * (0.25 + 1.00);
        EXPECT_NEAR(surface.total_variance(k, T_mid), 0.5 * (w0 + w1), 1e-12);
    }

    TEST(SVISurface, KDerivativesMatchIndependentFiniteDifferencesOfTotalVariance)
    {
        const SVISurface surface = two_slice_surface();
        const Real h = 1e-6;
        for (const Real T : {0.25, 0.5, 0.75, 1.00})
        {
            for (const Real k : {-0.2, 0.0, 0.2})
            {
                const Real fd1 = (surface.total_variance(k + h, T) - surface.total_variance(k - h, T)) / (2.0 * h);
                EXPECT_NEAR(surface.total_variance_dk(k, T), fd1, 1e-5);

                const Real h2 = 1e-4;
                const Real fd2 = (surface.total_variance_dk(k + h2, T) - surface.total_variance_dk(k - h2, T)) / (2.0 * h2);
                EXPECT_NEAR(surface.total_variance_dk2(k, T), fd2, 1e-4);
            }
        }
    }

    TEST(SVISurface, TDerivativeMatchesAnIndependentFiniteDifferenceInsideASegment)
    {
        const SVISurface surface = two_slice_surface();
        const Real k = 0.05;
        const Real T = 0.6; // strictly inside the only segment, away from either node
        const Real h = 1e-5;
        const Real fd = (surface.total_variance(k, T + h) - surface.total_variance(k, T - h)) / (2.0 * h);
        EXPECT_NEAR(surface.total_variance_dT(k, T), fd, 1e-6);
    }

    TEST(SVISurface, ClampsQueriesOutsideItsMaturityRange)
    {
        const SVISurface surface = two_slice_surface();
        const Real k = 0.1;
        EXPECT_NEAR(surface.total_variance(k, 5.0), surface.total_variance(k, surface.ttm_max()), 1e-12);
        EXPECT_NEAR(surface.total_variance(k, -1.0), surface.total_variance(k, surface.ttm_min()), 1e-12);
    }

    TEST(SVISurface, ImpliedVolIsTheSquareRootOfTotalVarianceOverT)
    {
        const SVISurface surface = two_slice_surface();
        const Real k = 0.0, T = 0.6;
        EXPECT_NEAR(surface.implied_vol(k, T), std::sqrt(surface.total_variance(k, T) / T), 1e-12);
    }

} // namespace quantModeling
