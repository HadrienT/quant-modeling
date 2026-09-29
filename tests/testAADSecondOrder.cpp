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

    namespace
    {
        // A call written as a fuzzy `if`: the call spread of half-width 1 around
        // the strike 100 (lot 16c) smooths the payoff (S - K) H_eps(S - K).
        const std::string kFuzzyCall = kOneYear + "\n    if spot() > 100 then\n        pays spot() - 100\n    endIf\n";
        constexpr double K = 100.0, EPS = 1.0;

        ScriptSettings fuzzy_settings()
        {
            ScriptSettings fuzzy;
            fuzzy.fuzzy = true;
            fuzzy.default_eps = EPS;
            return fuzzy;
        }

        double bs_gamma()
        {
            const double d1 = (std::log(S0 / K) + (R - Q + 0.5 * VOL * VOL)) / VOL;
            return std::exp(-Q) * norm_pdf(d1) / (S0 * VOL);
        }

        AADSecondOrderResults run_bumped(const std::string &script, std::size_t direction, std::size_t paths,
                                         ScriptSettings settings = {})
        {
            ScriptedProduct<aad::Number> product(script, kCtx, settings);
            BlackScholesSimModel<aad::Number> model{aad::Number(S0), aad::Number(R), aad::Number(Q),
                                                    aad::Number(VOL)};
            const auto r = simulate_aad_bumped_second_order(product, model, direction, paths, 11);
            EXPECT_EQ(model.parameters()[direction]->value(), direction == kSpot ? S0 : VOL); // restored
            return r;
        }
    } // namespace

    TEST(AADSecondOrder, AFuzzyCallsPathwiseGammaMissesTheKinksOfItsDerivative)
    {
        // P(x) = x H_eps(x), x = S_T - K, is continuous but P' jumps by
        // -1/2 at x = ±eps: Dirac masses in P''. Adjoint over tangent sees
        // only P'' = 1/eps inside the band, so it estimates
        //   e^{-rT} / (eps S0²) E[S_T² 1{K - eps < S_T < K + eps}]
        // (dS_T/dS0 = S_T/S0), which tends to twice the Black-Scholes gamma.
        // E[S_T² 1{S_T > a}] = S0² e^{(2(r - q) + σ²)T} N(d(a)),
        // d(a) = (ln(S0/a) + (r - q + 3σ²/2)T) / σ√T, with T = 1.
        const auto upper_tail = [](double a)
        { return norm_cdf((std::log(S0 / a) + (R - Q + 1.5 * VOL * VOL)) / VOL); };
        const double pathwise = std::exp(-R) / EPS * std::exp(2.0 * (R - Q) + VOL * VOL) *
                                (upper_tail(K - EPS) - upper_tail(K + EPS));
        const auto r = run(kFuzzyCall, kSpot, 100000, fuzzy_settings());
        EXPECT_NEAR(r.second[kSpot], pathwise, 4.0 * r.second_std_errors[kSpot]);
        EXPECT_NEAR(pathwise / bs_gamma(), 2.0, 0.01);
        const double d1 = (std::log(S0 / K) + (R - Q + 0.5 * VOL * VOL)) / VOL;
        EXPECT_NEAR(r.first, std::exp(-Q) * norm_cdf(d1), 4.0 * r.first_std_error + 0.005); // delta is fine
    }

    TEST(AADSecondOrder, TheBooksBumpedAdjointGammaOfAFuzzyCallIsBlackScholesGamma)
    {
        // §12.1: the delta of the smoothed call is continuous, so its central
        // difference converges -- to Black-Scholes up to O(eps²) and O(h²).
        const auto r = run_bumped(kFuzzyCall, kSpot, 100000, fuzzy_settings());
        EXPECT_NEAR(r.second[kSpot], bs_gamma(), 4.0 * r.second_std_errors[kSpot] + 0.01 * bs_gamma());
        EXPECT_LT(r.second_std_errors[kSpot], 0.05 * bs_gamma()); // common random numbers: a usable error
        // The same two runs give the whole row: vanna = -e^{-qT} n(d1) d2 / σ.
        const double d1 = (std::log(S0 / K) + (R - Q + 0.5 * VOL * VOL)) / VOL, d2 = d1 - VOL;
        const double vanna = -std::exp(-Q) * norm_pdf(d1) * d2 / VOL;
        EXPECT_NEAR(r.second[kVol], vanna, 4.0 * r.second_std_errors[kVol] + 0.02 * vanna);
    }

    TEST(AADSecondOrder, BothMethodsAgreeWhereThePayoffIsSmooth)
    {
        // S_T^1.5 is C¹ (C^∞) in every parameter: adjoint over tangent is exact
        // and the bumped rows converge to it on the same paths.
        const std::string script = kOneYear + "\n    pays spot() * sqrt(spot())\n";
        for (const std::size_t direction : {kSpot, kVol})
        {
            const auto tangent = run(script, direction, 20000);
            const auto bumped = run_bumped(script, direction, 20000);
            EXPECT_NEAR(bumped.price, tangent.price, 1e-9 * tangent.price);
            EXPECT_NEAR(bumped.first, tangent.first, 1e-9 * std::abs(tangent.first));
            for (const std::size_t j : {kSpot, kVol})
                EXPECT_NEAR(bumped.second[j], tangent.second[j],
                            2e-3 * std::abs(tangent.second[j]) + 2.0 * tangent.second_std_errors[j]);
        }
    }

} // namespace quantModeling
