#include <gtest/gtest.h>

#include "quantModeling/models/volatility.hpp"
#include "quantModeling/models/equity/black_scholes.hpp"
#include "quantModeling/models/equity/dupire.hpp"
#include "quantModeling/core/types.hpp"

#include <cmath>
#include <vector>

namespace quantModeling
{

    // ─────────────────────────────────────────────────────────────────────────
    //  FlatVol
    // ─────────────────────────────────────────────────────────────────────────

    TEST(FlatVol, ReturnsConstant)
    {
        FlatVol fv(0.25);
        EXPECT_DOUBLE_EQ(fv.value(100.0, 0.5), 0.25);
        EXPECT_DOUBLE_EQ(fv.value(50.0, 2.0), 0.25);
        EXPECT_DOUBLE_EQ(fv.value(200.0, 0.0), 0.25);
    }

    TEST(FlatVol, SigmaAccessor)
    {
        FlatVol fv(0.30);
        EXPECT_DOUBLE_EQ(fv.sigma(), 0.30);
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  GridLocalVol
    // ─────────────────────────────────────────────────────────────────────────

    TEST(GridLocalVol, ConstantSurface)
    {
        // 2×2 grid with constant vol → value(S,t) should return that constant
        GridLocalVol grid({80.0, 120.0}, {0.5, 1.5}, {0.2, 0.2, 0.2, 0.2});

        EXPECT_NEAR(grid.value(100.0, 1.0), 0.2, 1e-10);
        EXPECT_NEAR(grid.value(80.0, 0.5), 0.2, 1e-10);
        EXPECT_NEAR(grid.value(120.0, 1.5), 0.2, 1e-10);
    }

    TEST(GridLocalVol, LinearInterpolationK)
    {
        // Vol at K=80 is 0.20, at K=120 is 0.30, T is constant
        //   →  at K=100 (midpoint): 0.25
        GridLocalVol grid({80.0, 120.0}, {0.5, 1.5},
                          {0.20, 0.20,   // K=80, T=0.5 and T=1.5
                           0.30, 0.30}); // K=120, T=0.5 and T=1.5

        EXPECT_NEAR(grid.value(100.0, 1.0), 0.25, 1e-10);
    }

    TEST(GridLocalVol, LinearInterpolationT)
    {
        // Vol at T=0.5 is 0.20, at T=1.5 is 0.30, K is constant
        GridLocalVol grid({80.0, 120.0}, {0.5, 1.5},
                          {0.20, 0.30,   // K=80
                           0.20, 0.30}); // K=120

        EXPECT_NEAR(grid.value(100.0, 1.0), 0.25, 1e-10);
    }

    TEST(GridLocalVol, BilinearInterpolation)
    {
        // (K=80,T=0.5)=0.20, (K=80,T=1.5)=0.22, (K=120,T=0.5)=0.28, (K=120,T=1.5)=0.30
        GridLocalVol grid({80.0, 120.0}, {0.5, 1.5},
                          {0.20, 0.22,   // K=80
                           0.28, 0.30}); // K=120

        // At K=100 (wK=0.5), T=1.0 (wT=0.5):
        // (1-0.5)*(1-0.5)*0.20 + 0.5*(1-0.5)*0.28 + (1-0.5)*0.5*0.22 + 0.5*0.5*0.30
        // = 0.25*0.20 + 0.25*0.28 + 0.25*0.22 + 0.25*0.30
        // = 0.05 + 0.07 + 0.055 + 0.075 = 0.25
        EXPECT_NEAR(grid.value(100.0, 1.0), 0.25, 1e-10);
    }

    TEST(GridLocalVol, FlatExtrapolation)
    {
        GridLocalVol grid({80.0, 120.0}, {0.5, 1.5}, {0.2, 0.2, 0.2, 0.2});

        // Below grid: clamped to grid boundary
        EXPECT_NEAR(grid.value(50.0, 0.1), 0.2, 1e-10);
        // Above grid
        EXPECT_NEAR(grid.value(200.0, 5.0), 0.2, 1e-10);
    }

    TEST(GridLocalVol, VolShift)
    {
        GridLocalVol grid({80.0, 120.0}, {0.5, 1.5}, {0.2, 0.2, 0.2, 0.2}, 0.05);
        EXPECT_NEAR(grid.value(100.0, 1.0), 0.25, 1e-10);
    }

    TEST(GridLocalVol, ShiftedCopy)
    {
        GridLocalVol base({80.0, 120.0}, {0.5, 1.5}, {0.2, 0.2, 0.2, 0.2});
        auto shifted = base.shifted(0.01);
        EXPECT_NEAR(shifted.value(100.0, 1.0), 0.21, 1e-10);
        // Original unchanged
        EXPECT_NEAR(base.value(100.0, 1.0), 0.20, 1e-10);
    }

    TEST(GridLocalVol, MinimumVolFloor)
    {
        // Negative shift should not produce negative vol — floor at 1e-6
        GridLocalVol base({80.0, 120.0}, {0.5, 1.5}, {0.01, 0.01, 0.01, 0.01});
        auto shifted = base.shifted(-0.05);
        EXPECT_GE(shifted.value(100.0, 1.0), 1e-6);
    }

    TEST(GridLocalVol, ValidationTooFewNodes)
    {
        // Need at least 2 K and 2 T nodes
        EXPECT_THROW(
            GridLocalVol({100.0}, {0.5, 1.5}, {0.2, 0.2}),
            std::invalid_argument);
    }

    TEST(GridLocalVol, ValidationSizeMismatch)
    {
        EXPECT_THROW(
            GridLocalVol({80.0, 120.0}, {0.5, 1.5}, {0.2, 0.2, 0.2}),
            std::invalid_argument);
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  BlackScholesModel
    // ─────────────────────────────────────────────────────────────────────────

    TEST(BlackScholesModel, BasicAccessors)
    {
        BlackScholesModel m(100.0, 0.05, 0.02, 0.20);
        EXPECT_DOUBLE_EQ(m.spot0(), 100.0);
        EXPECT_DOUBLE_EQ(m.rate_r(), 0.05);
        EXPECT_DOUBLE_EQ(m.yield_q(), 0.02);
        EXPECT_DOUBLE_EQ(m.vol_sigma(), 0.20);
    }

    TEST(BlackScholesModel, VolSurfaceIsFlat)
    {
        BlackScholesModel m(100.0, 0.05, 0.02, 0.25);
        EXPECT_DOUBLE_EQ(m.vol().value(80.0, 0.5), 0.25);
        EXPECT_DOUBLE_EQ(m.vol().value(120.0, 2.0), 0.25);
    }

    TEST(BlackScholesModel, ModelName)
    {
        BlackScholesModel m(100.0, 0.05, 0.02, 0.20);
        EXPECT_EQ(m.model_name(), "BlackScholesModel");
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  DupireModel
    // ─────────────────────────────────────────────────────────────────────────

    TEST(DupireModel, BasicAccessors)
    {
        DupireModel m(100.0, 0.05, 0.02,
                      {80.0, 120.0}, {0.5, 1.5}, {0.2, 0.2, 0.2, 0.2});
        EXPECT_DOUBLE_EQ(m.spot0(), 100.0);
        EXPECT_DOUBLE_EQ(m.rate_r(), 0.05);
        EXPECT_DOUBLE_EQ(m.yield_q(), 0.02);
    }

    TEST(DupireModel, VolSigmaATM)
    {
        // vol_sigma() returns surface_.value(s0_, 0.0)
        // With flat grid = 0.20, this should be 0.20
        DupireModel m(100.0, 0.05, 0.02,
                      {80.0, 120.0}, {0.5, 1.5}, {0.2, 0.2, 0.2, 0.2});
        EXPECT_NEAR(m.vol_sigma(), 0.2, 1e-10);
    }

    TEST(DupireModel, VolSurfaceInterpolates)
    {
        DupireModel m(100.0, 0.05, 0.02,
                      {80.0, 120.0}, {0.5, 1.5},
                      {0.20, 0.22,   // K=80
                       0.28, 0.30}); // K=120

        EXPECT_NEAR(m.vol().value(100.0, 1.0), 0.25, 1e-10);
    }

    TEST(DupireModel, ModelName)
    {
        DupireModel m(100.0, 0.05, 0.02,
                      {80.0, 120.0}, {0.5, 1.5}, {0.2, 0.2, 0.2, 0.2});
        EXPECT_EQ(m.model_name(), "DupireModel");
    }

} // namespace quantModeling
