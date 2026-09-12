#include <gtest/gtest.h>

#include "quantModeling/models/equity/heston.hpp"

#include <cmath>

namespace quantModeling
{
    namespace
    {
        constexpr Real kTtm = 0.5;
    } // namespace

    // ── Feller condition ─────────────────────────────────────────────────────

    TEST(Heston, FellerConditionHoldsWhenTwoKappaThetaExceedsXiSquared)
    {
        HestonParams p;
        p.kappa = 1.5;
        p.theta = 0.04;
        p.xi = 0.3; // 2*1.5*0.04 = 0.12 > 0.09 = 0.3^2
        EXPECT_TRUE(feller_condition_satisfied(p));
    }

    TEST(Heston, FellerConditionFailsWhenXiIsLarge)
    {
        HestonParams p;
        p.kappa = 1.5;
        p.theta = 0.04;
        p.xi = 0.9; // 2*1.5*0.04 = 0.12 < 0.81 = 0.9^2
        EXPECT_FALSE(feller_condition_satisfied(p));
    }

    // ── Characteristic function sanity ───────────────────────────────────────

    TEST(Heston, CharacteristicFunctionAtZeroIsOne)
    {
        const HestonParams p{0.04, 1.5, 0.04, 0.3, -0.6};
        const auto phi0 = heston_log_return_characteristic_function(0.0, kTtm, p);
        EXPECT_NEAR(phi0.real(), 1.0, 1e-12);
        EXPECT_NEAR(phi0.imag(), 0.0, 1e-12);
    }

    TEST(Heston, CharacteristicFunctionModulusIsAtMostOne)
    {
        // |E[e^{iuX}]| <= E[|e^{iuX}|] = 1 for any real u -- a basic property
        // of characteristic functions, independent of the specific model.
        const HestonParams p{0.05, 2.0, 0.06, 0.4, -0.5};
        for (const Real u : {-20.0, -5.0, -1.0, 0.5, 3.0, 15.0})
            EXPECT_LE(std::abs(heston_log_return_characteristic_function(u, 1.0, p)), 1.0 + 1e-9);
    }

    TEST(Heston, CharacteristicFunctionIsConjugateSymmetric)
    {
        // phi(-u) = conj(phi(u)) since the underlying random variable
        // (log-return) is real-valued.
        const HestonParams p{0.04, 1.2, 0.05, 0.35, 0.3};
        for (const Real u : {0.7, 2.3, 8.0})
        {
            const auto phi_u = heston_log_return_characteristic_function(u, kTtm, p);
            const auto phi_mu = heston_log_return_characteristic_function(-u, kTtm, p);
            EXPECT_NEAR(phi_u.real(), phi_mu.real(), 1e-9);
            EXPECT_NEAR(phi_u.imag(), -phi_mu.imag(), 1e-9);
        }
    }

} // namespace quantModeling
