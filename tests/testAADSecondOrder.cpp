#include <gtest/gtest.h>

#include "quantModeling/aad/tangent.hpp"
#include "quantModeling/core/date.hpp"
#include "quantModeling/engines/mc/simulation_engine_aad2.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/market/valuation_context.hpp"
#include "quantModeling/models/equity/bs_sim_model.hpp"
#include "quantModeling/utils/stats.hpp"

#include <cmath>
#include <string>

namespace quantModeling
{
    namespace
    {
        using N2 = SecondOrderNumber;

        const ValuationContext kCtx{Date::from_iso("2024-06-03")};
        const std::string kOneYear = "2025-06-03"; // 365 days: T = 1 under ACT/365F

        constexpr double S0 = 100.0, R = 0.03, Q = 0.01, VOL = 0.25;

        AADSecondOrderResults run(const std::string &script, std::size_t direction, std::size_t paths,
                                  ScriptSettings settings = {})
        {
            ScriptedProduct<N2> product(script, kCtx, settings);
            BlackScholesSimModel<N2> model{N2(S0), N2(R), N2(Q), N2(VOL)};
            return simulate_aad_second_order(product, model, direction, paths, 11);
        }

        // Parameter order of BlackScholesSimModel: spot, rate, div, vol.
        constexpr std::size_t kSpot = 0, kVol = 3;
    } // namespace

    TEST(AADTangent, DualNumbersOnDoublesDifferentiateExactly)
    {
        using D = aad::Tangent<double>;
        const D x{2.0, 1.0};
        const D y = exp(x * x) / sqrt(x) + pow(x, 3.0) - max(x, 1.5) * log(x);
        const double f_prime = std::exp(4.0) * (2.0 * 2.0 / std::sqrt(2.0) - 0.5 / std::pow(2.0, 1.5)) +
                               3.0 * 4.0 - (std::log(2.0) + 1.0);
        EXPECT_NEAR(y.d, f_prime, 1e-9 * std::abs(f_prime));
    }

    TEST(AADSecondOrder, SquaredSpotMatchesItsClosedFormHessian)
    {
        // V = e^{-rT} S0² e^{(2(r - q) + σ²)T}, T = 1: the pathwise second
        // derivatives of S_T² are exact, so the Monte-Carlo means converge to
        // the closed forms.
        const std::string script = kOneYear + "\n    pays spot() * spot()\n";
        const double g = std::exp(-R) * std::exp(2.0 * (R - Q) + VOL * VOL);
        const auto by_spot = run(script, kSpot, 40000);
        EXPECT_NEAR(by_spot.price, S0 * S0 * g, 4.0 * by_spot.price_std_error);
        EXPECT_NEAR(by_spot.first, 2.0 * S0 * g, 4.0 * by_spot.first_std_error);             // delta
        EXPECT_NEAR(by_spot.second[kSpot], 2.0 * g, 4.0 * by_spot.second_std_errors[kSpot]); // gamma
        EXPECT_NEAR(by_spot.second[kVol], 2.0 * S0 * 2.0 * VOL * g,
                    4.0 * by_spot.second_std_errors[kVol]); // vanna
        const auto by_vol = run(script, kVol, 40000);
        EXPECT_NEAR(by_vol.second[kVol], S0 * S0 * g * (2.0 + 4.0 * VOL * VOL),
                    4.0 * by_vol.second_std_errors[kVol]); // volga
    }

    TEST(AADSecondOrder, TheHessianIsSymmetricPathByPath)
    {
        // d²P/dS dσ from the spot direction and d²P/dσ dS from the vol
        // direction are the same sum on the same paths: equal to rounding.
        const std::string script = kOneYear + "\n    pays spot() * sqrt(spot())\n";
        const auto a = run(script, kSpot, 2000);
        const auto b = run(script, kVol, 2000);
        EXPECT_NEAR(a.second[kVol], b.second[kSpot], 1e-10 * std::abs(a.second[kVol]));
    }

    TEST(AADSecondOrder, SmoothedCallGammaIsBlackScholesGamma)
    {
        // A hard max has no pathwise gamma; the fuzzy evaluator's call-spread
        // smoothing (half-width 1 on a strike of 100) has, biased by O(eps²).
        const std::string script = kOneYear + "\n    if spot() > 100 then\n        pays spot() - 100\n    endIf\n";
        ScriptSettings fuzzy;
        fuzzy.fuzzy = true;
        fuzzy.default_eps = 1.0;
        const auto r = run(script, kSpot, 100000, fuzzy);
        const double d1 = (std::log(S0 / 100.0) + (R - Q + 0.5 * VOL * VOL)) / VOL;
        const double gamma = std::exp(-Q) * norm_pdf(d1) / (S0 * VOL);
        EXPECT_NEAR(r.second[kSpot], gamma, 4.0 * r.second_std_errors[kSpot] + 0.01 * gamma);
        EXPECT_NEAR(r.first, std::exp(-Q) * norm_cdf(d1), 4.0 * r.first_std_error + 0.005);
    }

} // namespace quantModeling
