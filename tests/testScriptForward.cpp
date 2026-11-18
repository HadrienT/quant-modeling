#include <gtest/gtest.h>

#include "quantModeling/aad/number.hpp"
#include "quantModeling/core/date.hpp"
#include "quantModeling/engines/mc/simulation_engine.hpp"
#include "quantModeling/engines/mc/lsm.hpp"
#include "quantModeling/engines/mc/simulation_engine_aad.hpp"
#include "quantModeling/gpu/script.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/market/valuation_context.hpp"
#include "quantModeling/models/equity/bs_sim_model.hpp"
#include "quantModeling/models/equity/multi_asset_bs_sim_model.hpp"
#include "quantModeling/scripting/script_model_factory.hpp"
#include "quantModeling/utils/stats.hpp"

#include <Eigen/Core>
#include <cmath>
#include <cstdlib>
#include <string>

// The language's dividends and rates (issue 313's follow-up): fwd(DATE), the
// model's forward of the underlying; yf(D1, D2), the script's year fraction;
// fwdRate(S, E) = (df(S) / df(E) - 1) / yf(S, E), the curve's simple rate.
// The oracles are the closed forms each one stands for.

namespace quantModeling
{
    namespace
    {
        ValuationContext ctx()
        {
            return ValuationContext{Date::from_iso("2024-06-03")};
        }
        Time t(const char *iso)
        {
            return ctx().t(Date::from_iso(iso));
        }

        PricingSettings settings(int paths = 200000)
        {
            PricingSettings s;
            s.mc_paths = paths;
            s.mc_seed = 7;
            s.mc_antithetic = true;
            return s;
        }

        double bs_call(double S, double K, double r, double q, double v, double T)
        {
            const double sd = v * std::sqrt(T);
            const double d1 = (std::log(S / K) + (r - q + 0.5 * v * v) * T) / sd;
            return S * std::exp(-q * T) * norm_cdf(d1) - K * std::exp(-r * T) * norm_cdf(d1 - sd);
        }
    } // namespace

    // fwd(T) seen at t is S_t e^((r - q)(T - t)): paid at t with df(T), the
    // holding of the stock to T, worth S0 e^(-q T) today.
    TEST(ScriptForward, FwdIsTheModelsForwardAndCarriesTheDividend)
    {
        const double S0 = 100, r = 0.03, q = 0.04, v = 0.25;
        ScriptedProduct<Real> prod("2025-06-03\n    pays fwd(2026-06-03) * df(2026-06-03)\n", ctx());
        BlackScholesSimModel<Real> model(S0, r, q, v);
        const auto res = simulate<Real>(prod, model, settings());
        const double expected = S0 * std::exp(-q * t("2026-06-03"));
        EXPECT_NEAR(res.npv(), expected, 4.0 * res.std_error());
        EXPECT_LT(res.std_error(), 0.05 * expected);
    }

    // The bytecode reads the forwards as the tree evaluator does: the same
    // bits, the single-path machine and the batched one alike.
    TEST(ScriptForward, TheBytecodeIsTheTreeEvaluatorBitForBit)
    {
        const std::string src = "2025-01-03\n    s1 = spot()\n"
                                "2025-06-03\n    pays max(fwd(2026-06-03) - s1, 0) * df(2026-06-03)\n";
        ScriptSettings tree, code;
        tree.bytecode = false;
        code.bytecode = true;
        const ScriptedProduct<Real> a(src, ctx(), tree), b(src, ctx(), code);
        ASSERT_FALSE(b.program().code.empty());
        ASSERT_TRUE(b.program().has_forward);
        BlackScholesSimModel<Real> m1(100, 0.03, 0.02, 0.2), m2(100, 0.03, 0.02, 0.2);
        const auto ra = simulate<Real>(a, m1, settings(20000)), rb = simulate<Real>(b, m2, settings(20000));
        EXPECT_EQ(ra.npv(), rb.npv());
        EXPECT_EQ(ra.std_error(), rb.std_error());
    }

    // The tape differentiates through the forward: d/dq of S0 e^(-q T) is
    // -T S0 e^(-q T), and d/dS0 is e^(-q T).
    TEST(ScriptForward, TheAdjointSeesTheDividendThroughTheForward)
    {
        using aad::Number;
        aad::Tape tape;
        Number::tape = &tape;
        const double S0 = 100, r = 0.03, q = 0.04, v = 0.25;
        ScriptedProduct<Number> prod("2025-06-03\n    pays fwd(2026-06-03) * df(2026-06-03)\n", ctx());
        BlackScholesSimModel<Number> model{Number(S0), Number(r), Number(q), Number(v)};
        const AADSimulResults res = simulate_aad(prod, model, 100000, 3);
        Number::tape = nullptr;
        const double T = t("2026-06-03");
        for (std::size_t i = 0; i < res.risk_labels.size(); ++i)
        {
            if (res.risk_labels[i] == "div")
                EXPECT_NEAR(res.risks[i], -T * S0 * std::exp(-q * T), 4.0 * res.risk_std_errors[i] + 1e-6);
            if (res.risk_labels[i] == "spot")
                EXPECT_NEAR(res.risks[i], std::exp(-q * T), 4.0 * res.risk_std_errors[i] + 1e-9);
        }
    }

    // yf() is the context's year fraction; a constant.
    TEST(ScriptForward, YfIsTheScriptsYearFraction)
    {
        ScriptedProduct<Real> prod("2025-06-03\n    pays 1000 * yf(2025-06-03, 2025-12-03)\n", ctx());
        BlackScholesSimModel<Real> model(100, 0.0, 0.0, 0.2);
        const auto res = simulate<Real>(prod, model, settings(100));
        EXPECT_NEAR(res.npv(), 1000.0 * (t("2025-12-03") - t("2025-06-03")), 1e-9);
        EXPECT_NEAR(res.std_error(), 0.0, 1e-12);
    }

    // Under a flat rate the simple forward rate is (e^(r tau) - 1) / tau.
    TEST(ScriptForward, FwdRateIsTheCurvesSimpleRate)
    {
        const double r = 0.04;
        ScriptedProduct<Real> prod("2025-06-03\n    pays fwdRate(2025-09-03, 2025-12-03)\n", ctx());
        BlackScholesSimModel<Real> model(100, r, 0.0, 0.2);
        const auto res = simulate<Real>(prod, model, settings(100));
        const double tau = t("2025-12-03") - t("2025-09-03");
        const double rate = (std::exp(r * tau) - 1.0) / tau;
        EXPECT_NEAR(res.npv(), rate * std::exp(-r * t("2025-06-03")), 1e-12);
    }

    // A forward-rate agreement under curve-fitted Hull-White, settled at its
    // reset S on (L(S, E) - K) yf df(E): worth P(0, S) - P(0, E) (1 + K yf),
    // whatever the short rate's dynamics -- the curve alone prices it.
    TEST(ScriptForward, AForwardRateAgreementIsPricedByTheCurveAlone)
    {
        scripting::ScriptModelSpec spec;
        spec.model = "hull_white";
        spec.spot = 100.0;
        spec.vol = 0.2;
        spec.hw_mean_reversion = 0.05;
        spec.hw_sigma = 0.012;
        spec.curve_times = {0.5, 1.0, 2.0, 3.0, 5.0};
        spec.curve_dfs = {0.985, 0.968, 0.935, 0.902, 0.84};
        const double K = 0.035;
        const std::string src = "2025-12-03\n    pays (fwdRate(2025-12-03, 2026-06-03) - " + std::to_string(K) +
                                ") * yf(2025-12-03, 2026-06-03) * df(2026-06-03)\n";
        ScriptedProduct<Real> prod(src, ctx());
        auto model = scripting::make_script_model<Real>(spec);
        const auto res = simulate<Real>(prod, *model, settings());
        const auto P = [&](double x)
        {
            // log-linear between the pillars, the curve's own interpolation
            const auto &T = spec.curve_times, &D = spec.curve_dfs;
            std::size_t j = 1;
            while (j + 1 < T.size() && T[j] < x)
                ++j;
            const double w = (x - T[j - 1]) / (T[j] - T[j - 1]);
            return std::exp((1 - w) * std::log(D[j - 1]) + w * std::log(D[j]));
        };
        const double S = t("2025-12-03"), E = t("2026-06-03");
        const double expected = P(S) - P(E) * (1.0 + K * (E - S));
        EXPECT_NEAR(res.npv(), expected, 4.0 * res.std_error() + 2e-5);
    }

    // An American call on a dividend payer: exercise considered only in the
    // money and above the forward held to the expiry -- worth at least the
    // European, at most the American.
    TEST(ScriptForward, TheForwardBoundKeepsAnAmericanCallAboveTheEuropean)
    {
        const double S0 = 100, r = 0.05, q = 0.04, v = 0.2;
        std::string src = "2024-06-10\n    done = 0\n\n"
                          "schedule(2024-06-17, 2025-05-26, 1W, NONE, U)\n"
                          "    if spot() > 100 and spot() - 100 > fwd(2025-06-03) * df(2025-06-03) - 100 * "
                          "df(2025-06-03) and exercise(spot() / 100) then\n"
                          "        pays spot() - 100\n        done = 1\n    endIf\n\n"
                          "2025-06-03\n    if done = 0 then\n        pays max(spot() - 100, 0)\n    endIf\n";
        ScriptedProduct<Real> prod(src, ctx());
        BlackScholesSimModel<Real> model(S0, r, q, v);
        fit_exercise_policy(prod, model, 100000, 5);
        const auto res = simulate<Real>(prod, model, settings(100000));
        const double european = bs_call(S0, 100, r, q, v, t("2025-06-03"));
        EXPECT_GT(res.npv(), european - 2.0 * res.std_error());
    }

    TEST(ScriptForward, MisplacedLookupsAreRefused)
    {
        // a forward for a date before the event
        EXPECT_THROW((ScriptedProduct<Real>("2025-06-03\n    pays fwd(2025-01-03)\n", ctx())), InvalidInput);
        // in a historical event
        EXPECT_THROW((ScriptedProduct<Real>("2024-01-03\n    x = fwd(2025-01-01)\n2025-06-03\n    pays x\n", ctx(),
                                            {}, {{Date::from_iso("2024-01-03"), {100.0}}})),
                     InvalidInput);
        // yf and fwdRate need START before END
        EXPECT_ANY_THROW((ScriptedProduct<Real>("2025-06-03\n    pays yf(2025-12-03, 2025-06-03)\n", ctx())));
    }

    // The multi-asset models fill fwd() for spot(0), as spot() is spot(0),
    // and now df() at their flat rate, as the GPU computes it.
    TEST(ScriptForward, MultiAssetModelsFillTheForwardOfSpot0AndDf)
    {
        Eigen::MatrixXd corr(2, 2);
        corr << 1.0, 0.5, 0.5, 1.0;
        const double r = 0.03;
        MultiAssetBSSimModel<Real> model(std::vector<Real>{100.0, 100.0}, r, std::vector<Real>{0.0, 0.0},
                                         std::vector<Real>{0.2, 0.2}, corr);
        // asset 0 pays a 3 % dividend: holding it to T is worth S0 e^(-q0 T)
        MultiAssetBSSimModel<Real> divs(std::vector<Real>{100.0, 80.0}, r, std::vector<Real>{0.03, 0.0},
                                        std::vector<Real>{0.2, 0.3}, corr);
        ScriptedProduct<Real> held("2025-06-03\n    pays fwd(2026-06-03) * df(2026-06-03) + 0 * spot(1)\n", ctx());
        const auto h = simulate<Real>(held, divs, settings());
        EXPECT_NEAR(h.npv(), 100.0 * std::exp(-0.03 * t("2026-06-03")), 4.0 * h.std_error());
        ScriptedProduct<Real> good("2025-06-03\n    pays 1000 * df(2026-06-03) + 0 * spot(1)\n", ctx());
        MultiAssetBSSimModel<Real> model2(std::vector<Real>{100.0, 100.0}, r, std::vector<Real>{0.0, 0.0},
                                          std::vector<Real>{0.2, 0.2}, corr);
        const auto res = simulate<Real>(good, model2, settings(100));
        EXPECT_NEAR(res.npv(), 1000.0 * std::exp(-r * t("2026-06-03")), 1e-9);
    }

    // The IR carries fwd() and each event's maturities (the kernels compiled
    // from it bind spot(0) e^((r - q) tau)); the interpreted kernels do not
    // read forwards, so with compilation off the GPU refuses it.
    TEST(ScriptForward, TheIrCarriesTheForwardAndItsMaturities)
    {
        const ScriptedProduct<Real> prod("2025-06-03\n    pays fwd(2026-06-03) + fwd(2025-12-03)\n", ctx());
        ASSERT_FALSE(prod.ir().empty()) << prod.ir_refusal();
        EXPECT_TRUE(prod.ir().has_forward);
        ASSERT_EQ(prod.ir().event_fwd_mats.size(), 1u);
        const std::vector<Time> mats{t("2026-06-03"), t("2025-12-03")};
        EXPECT_EQ(prod.ir().event_fwd_mats[0], mats);
        DeviceModel dm;
        setenv("QM_SCRIPT_JIT", "0", 1);
        const std::string why = gpu::script_gpu_unsupported(prod.program(), dm, {});
        unsetenv("QM_SCRIPT_JIT");
        EXPECT_NE(why.find("fwd()"), std::string::npos) << why;
    }

} // namespace quantModeling
