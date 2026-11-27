#include <gtest/gtest.h>

#include "quantModeling/core/date.hpp"
#include "quantModeling/engines/analytic/curve_closed_forms.hpp"
#include "quantModeling/engines/mc/script_engine.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/market/forward_curve.hpp"
#include "quantModeling/market/valuation_context.hpp"
#include "quantModeling/models/equity/sabr.hpp"
#include "quantModeling/pricers/script_builders.hpp"
#include "quantModeling/scripting/script_model_factory.hpp"

#include <cmath>
#include <functional>
#include <string>
#include <vector>

// WP 32 F6: Black-Scholes closed forms on a ForwardCurve, against the Monte
// Carlo on the same curve, their pillar risks against bumps, and the control
// variates that read them (blueprint/wp/32-forward-curve.md).

namespace quantModeling
{
    namespace
    {
        const double S0 = 100.0, kVol = 0.2, kDay = 1.0 / 365.0;

        // testForwardCurveModels.cpp's SPX-like curve: a rate from 4.4 % to
        // 5.1 %, a 1.2 % yield, a cash dividend of 1.5 on day 91 as two
        // pillars a second apart.
        ForwardCurve market_curve()
        {
            const double tau = 91.0 * kDay, gap = kDay / 86400.0;
            const std::vector<Time> T{2.0 / 12, tau, tau + gap, 0.5, 1.0, 2.0};
            const std::vector<Real> r{0.044, 0.045, 0.045, 0.047, 0.050, 0.051};
            std::vector<Real> F, DF;
            for (std::size_t i = 0; i < T.size(); ++i)
            {
                const double fwd = S0 * std::exp((r[i] - 0.012) * T[i]);
                F.push_back(T[i] > tau ? fwd - 1.5 * std::exp((r[i] - 0.012) * (T[i] - tau)) : fwd);
                DF.push_back(std::exp(-r[i] * T[i]));
            }
            return ForwardCurve::from_pillars(S0, T, F, DF);
        }

        // One pillar bumped: its zero rate by h (DF and F together, as the
        // scalar rate), or its dividend yield by h (F alone).
        ForwardCurve bumped(const ForwardCurve &c, std::size_t j, bool rate, double h)
        {
            std::vector<Time> T = c.pillar_times();
            std::vector<Real> F, DF = c.discount_curve().pillar_discount_factors();
            for (const Real y : c.pillar_log_forwards())
                F.push_back(c.spot() * std::exp(y));
            if (rate)
            {
                DF[j] *= std::exp(-h * T[j]);
                F[j] *= std::exp(h * T[j]);
            }
            else
                F[j] *= std::exp(-h * T[j]);
            return ForwardCurve::from_pillars(c.spot(), T, F, DF);
        }

        using Pricer = std::function<CurveClosedForm(const ForwardCurve &)>;

        // Fixings on both sides of the dividend, one past the last pillar.
        const std::vector<Time> kFixings{30 * kDay, 61 * kDay, 91 * kDay, 92 * kDay, 146 * kDay, 800 * kDay};

        std::vector<std::pair<std::string, Pricer>> pricers()
        {
            const std::vector<Time> early(kFixings.begin(), kFixings.begin() + 5);
            return {
                {"call 0.4y", [](const ForwardCurve &c)
                 { return black_scholes_vanilla(c, kVol, 101.0, 0.4, true); }},
                {"put 2.5y", [](const ForwardCurve &c)
                 { return black_scholes_vanilla(c, kVol, 95.0, 2.5, false); }},
                {"digital 0.3y",
                 [](const ForwardCurve &c)
                 { return black_scholes_digital(c, kVol, 99.0, 0.3, true, 10.0); }},
                {"forward 1.5y", [](const ForwardCurve &c)
                 { return equity_forward(c, 98.0, 1.5); }},
                {"geometric Asian",
                 [](const ForwardCurve &c)
                 { return geometric_asian(c, kVol, 100.0, true, kFixings, 2.3); }},
                {"arithmetic Asian",
                 [early](const ForwardCurve &c)
                 { return arithmetic_asian_levy(c, kVol, 100.0, false, early, 0.45); }},
            };
        }

        // simulate_script under Black-Scholes on the curve.
        SimulationMCResult simulate_on(const ForwardCurve &c, ScriptedProduct<Real> &product, int paths = 400000)
        {
            scripting::ScriptModelSpec s;
            s.model = "black_scholes";
            s.spot = S0;
            s.vol = kVol;
            s.max_dt = 1.0 / 52.0;
            s.forward_curve = c;
            auto model = scripting::make_script_model<Real>(s);
            PricingSettings settings;
            settings.mc_paths = paths;
            return simulate_script(product, *model, settings);
        }

        const ValuationContext kCtx{Date::from_iso("2026-06-01")};
        double t_of(const char *iso)
        {
            return kCtx.t(Date::from_iso(iso));
        }
    } // namespace

    // A flat curve gives the scalar closed forms.
    TEST(CurveClosedForms, FlatCurveIsTheScalarFormula)
    {
        const double r = 0.045, q = 0.012, T = 1.3, K = 103.0;
        const ForwardCurve flat = ForwardCurve::flat(S0, r, q);
        const double F = S0 * std::exp((r - q) * T), DF = std::exp(-r * T);
        EXPECT_NEAR(black_scholes_vanilla(flat, kVol, K, T, true).price, black76_call_price(F, K, T, kVol, DF), 1e-12);
        EXPECT_NEAR(black_scholes_vanilla(flat, kVol, K, T, false).price,
                    black76_call_price(F, K, T, kVol, DF) - DF * (F - K), 1e-12);
        EXPECT_NEAR(geometric_asian(flat, kVol, K, true, kFixings, kFixings.back()).price,
                    script_builders::geometric_asian_bs(S0, r, q, kVol, K, true, kFixings), 1e-12);
        // Call-put parity of the digitals: their sum is the zero coupon.
        EXPECT_NEAR(black_scholes_digital(flat, kVol, K, T, true, 1.0).price +
                        black_scholes_digital(flat, kVol, K, T, false, 1.0).price,
                    DF, 1e-14);
        EXPECT_TRUE(curve_pillar_risks(flat, black_scholes_vanilla(flat, kVol, K, T, true).sensitivity).empty());
    }

    // The closed forms on the curve are the Monte Carlo's on the curve; the
    // flat fit at the payment date misses those that read earlier dates.
    TEST(CurveClosedForms, ClosedFormsMatchTheMonteCarloOnTheCurve)
    {
        const ForwardCurve c = market_curve();
        const char *dates[] = {"2026-07-01", "2026-08-01", "2026-08-31", "2026-09-01", "2026-10-25"};
        std::vector<Time> t;
        for (const char *d : dates)
            t.push_back(t_of(d));
        const std::string n = std::to_string(t.size());
        auto asian = [&](bool geometric)
        {
            std::string s;
            for (std::size_t i = 0; i < t.size(); ++i)
            {
                s += (i == 0 ? "" : "\n") + std::string(dates[i]) + "\n";
                s += i == 0 ? "    acc = spot()\n    lacc = log(spot())\n"
                            : "    acc = acc + spot()\n    lacc = lacc + log(spot())\n";
            }
            return s + (geometric ? "    pays max(exp(lacc / " + n + ") - 100, 0)\n"
                                  : "    pays max(100 - acc / " + n + ", 0)\n");
        };
        struct Case
        {
            std::string script;
            double closed;
            double approx; // the closed form's own error (Levy's moment matching)
        };
        const Time T = t.back();
        const std::vector<Case> cases{
            {"2026-10-25\n    pays max(spot() - 101, 0)\n", black_scholes_vanilla(c, kVol, 101.0, T, true).price, 0},
            {"2026-10-25\n    if spot() < 99 then\n        pays 10\n    endIf\n", black_scholes_digital(c, kVol, 99.0, T, false, 10.0).price,
             0},
            {asian(true), geometric_asian(c, kVol, 100.0, true, t, T).price, 0},
            {asian(false), arithmetic_asian_levy(c, kVol, 100.0, false, t, T).price, 0.01},
        };
        for (const Case &k : cases)
        {
            ScriptedProduct<Real> product(k.script, kCtx);
            const SimulationMCResult r = simulate_on(c, product);
            EXPECT_NEAR(r.values[0], k.closed, 4.0 * r.std_errors[0] + k.approx) << k.script;
        }
        // The geometric Asian on the flat fit at T reads the wrong forwards
        // at its fixings before the dividend.
        const double r_T = -std::log(c.discount(T)) / T, q_T = r_T - std::log(c.forward(T) / S0) / T;
        EXPECT_GT(std::abs(script_builders::geometric_asian_bs(S0, r_T, q_T, kVol, 100.0, true, t) -
                           geometric_asian(c, kVol, 100.0, true, t, T).price),
                  0.05);
    }

    // The risk to each pillar is the bumped pillar's, and the pillars sum
    // to the parallel rho and dividend risk, which the shifted curve gives.
    TEST(CurveClosedForms, PillarRisksMatchBumpsAndSumToTheParallelRisks)
    {
        const ForwardCurve c = market_curve();
        const std::size_t np = c.pillar_times().size();
        const double h = 1e-6;
        for (const auto &[name, price] : pricers())
        {
            const CurveClosedForm v = price(c);
            const std::vector<PillarRisk> risks = curve_pillar_risks(c, v.sensitivity);
            ASSERT_EQ(risks.size(), 2 * np) << name;
            double zero_sum = 0.0, div_sum = 0.0;
            for (std::size_t j = 0; j < np; ++j)
            {
                for (const bool rate : {true, false})
                {
                    const double fd =
                        (price(bumped(c, j, rate, h)).price - price(bumped(c, j, rate, -h)).price) / (2.0 * h);
                    const PillarRisk &p = risks[rate ? j : np + j];
                    EXPECT_NEAR(p.value, fd, 1e-6 * (1.0 + std::abs(fd))) << name << " " << p.label;
                    (rate ? zero_sum : div_sum) += p.value;
                }
            }
            const auto [rho, dq] = curve_parallel_risks(v.sensitivity);
            EXPECT_NEAR(zero_sum, rho, 1e-10 * (1.0 + std::abs(rho))) << name;
            EXPECT_NEAR(div_sum, dq, 1e-10 * (1.0 + std::abs(dq))) << name;
            const double rho_fd = (price(c.shifted(h, 0.0)).price - price(c.shifted(-h, 0.0)).price) / (2.0 * h);
            const double dq_fd = (price(c.shifted(0.0, h)).price - price(c.shifted(0.0, -h)).price) / (2.0 * h);
            EXPECT_NEAR(rho, rho_fd, 1e-6 * (1.0 + std::abs(rho))) << name;
            EXPECT_NEAR(dq, dq_fd, 1e-6 * (1.0 + std::abs(dq))) << name;
        }
        EXPECT_EQ(curve_pillar_risks(c, black_scholes_vanilla(c, kVol, 100.0, 0.4, true).sensitivity)[0].label,
                  "zero[0.1666666667]");
    }

    // controlPays on a curve: the engine's expectation of the control is the
    // curve's, so the controlled price stays on the closed form. The flat
    // fit at the payment's date would give the control at an earlier date
    // the wrong mean, and the price its bias.
    TEST(CurveClosedForms, ControlVariateMeanIsTheCurves)
    {
        const ForwardCurve c = market_curve();
        const Time t1 = t_of("2026-08-15"), t2 = t_of("2026-11-15");
        ScriptedProduct<Real> product(
            "2026-08-15\n    controlPays max(spot() - 100, 0) + 0.5 * spot() + 2\n\n"
            "2026-11-15\n    pays max(spot() - 101, 0)\n",
            kCtx);
        const std::optional<double> mean = product.black_scholes_control_mean({S0}, 0.0, {0.0}, {kVol}, &c);
        ASSERT_TRUE(mean);
        const double expected = black_scholes_vanilla(c, kVol, 100.0, t1, true).price +
                                0.5 * c.discount(t1) * c.forward(t1) + 2.0 * c.discount(t1);
        EXPECT_NEAR(*mean, expected, 1e-12);
        product.set_control_mean(mean);
        const SimulationMCResult r = simulate_on(c, product, 100000);
        const double closed = black_scholes_vanilla(c, kVol, 101.0, t2, true).price;
        EXPECT_NEAR(r.values[0], closed, 4.0 * r.std_errors[0]) << r.diagnostics;

        // The flat (r, q) fitted at t2, which the binding passed before F6.
        const double r2 = -std::log(c.discount(t2)) / t2, q2 = r2 - std::log(c.forward(t2) / S0) / t2;
        const double stale = *product.black_scholes_control_mean({S0}, r2, {q2}, {kVol});
        EXPECT_GT(std::abs(stale - expected), 10.0 * r.std_errors[0]);
        // ... and with it, the controlled price leaves the closed form.
        product.set_control_mean(stale);
        const SimulationMCResult biased = simulate_on(c, product, 100000);
        EXPECT_GT(std::abs(biased.values[0] - closed), 8.0 * biased.std_errors[0]) << biased.diagnostics;
        // Several assets on one curve have no closed form here.
        EXPECT_FALSE(product.black_scholes_control_mean({S0, S0}, 0.0, {0.0, 0.0}, {kVol, kVol}, &c));
    }

    TEST(CurveClosedForms, RefusesDegenerateInputs)
    {
        const ForwardCurve c = market_curve();
        EXPECT_THROW(black_scholes_vanilla(c, kVol, 100.0, 0.0, true), InvalidInput);
        EXPECT_THROW(geometric_asian(c, kVol, 100.0, true, {}, 1.0), InvalidInput);
        EXPECT_THROW(geometric_asian(c, kVol, 100.0, true, {0.5, 0.2}, 1.0), InvalidInput);
        EXPECT_THROW(arithmetic_asian_levy(c, kVol, 100.0, true, {0.5}, 0.2), InvalidInput);
    }

} // namespace quantModeling
