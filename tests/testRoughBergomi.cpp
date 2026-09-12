#include <gtest/gtest.h>

#include "quantModeling/models/equity/rough_bergomi.hpp"

namespace quantModeling
{

    TEST(RoughBergomi, AlphaIsHurstMinusOneHalf)
    {
        RoughBergomiParams p;
        p.H = 0.1;
        EXPECT_DOUBLE_EQ(rough_bergomi_alpha(p), -0.4);
    }

    TEST(RoughBergomi, AlphaIsZeroAtTheBrownianBoundary)
    {
        // H = 1/2 is the boundary at which the Volterra process coincides
        // with standard Brownian motion (McCrickerd & Pakkanen, Figure 1).
        RoughBergomiParams p;
        p.H = 0.5;
        EXPECT_DOUBLE_EQ(rough_bergomi_alpha(p), 0.0);
    }

} // namespace quantModeling
