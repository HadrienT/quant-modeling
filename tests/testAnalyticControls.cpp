// Analytic control variates declared in scripts (WP 26 P2): `controlPays
// expr` accumulates a control payoff on the same path, and when the caller
// gives its expectation (ScriptSettings::control_mean) the engine regresses
// the price on it with the generic controls.
//
// Properties checked:
//  - parsing and writing back;
//  - without an expectation the control is computed and ignored: the price
//    is the plain script's, to the bit;
//  - the arithmetic Asian with the geometric Asian as its control is
//    unbiased (same price within error as without) and reaches the variance
//    of the hand-written Asian engine, which uses the same control -- P2's
//    acceptance criterion;
//  - the control is blended like any variable in a fuzzy if, and the tree
//    evaluator and the bytecode agree on it to the bit.

#include <gtest/gtest.h>

#include "quantModeling/core/date.hpp"
#include "quantModeling/engines/mc/simulation_engine.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/market/valuation_context.hpp"
#include "quantModeling/models/equity/bs_sim_model.hpp"
#include "quantModeling/pricers/context.hpp"
#include "quantModeling/pricers/registry.hpp"
#include "quantModeling/scripting/parser.hpp"
#include "quantModeling/scripting/visitors/script_writer.hpp"
#include "quantModeling/utils/stats.hpp"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace quantModeling
{
    namespace
    {
        constexpr double S0 = 100.0, K = 100.0, r = 0.05, q = 0.02, vol = 0.2;

        ValuationContext ctx()
        {
            return ValuationContext{Date::from_iso("2024-06-03")};
        }

        PricingSettings settings(int paths = 1 << 17)
        {
            PricingSettings s;
            s.mc_paths = paths;
            s.mc_seed = 20240603;
            return s;
        }

        bool same_bits(double a, double b)
        {
            return std::memcmp(&a, &b, sizeof a) == 0;
        }

        // Daily fixings over a year, the arithmetic average against the
        // strike; the geometric average of the same fixings as the control.
        const std::string kAsian = "2024-06-04\n    acc = spot()\n    lacc = log(spot())\n    n = 1\n\n"
                                   "schedule(2024-06-05, 2025-05-30, 1D, TARGET, F)\n"
                                   "    acc = acc + spot()\n    lacc = lacc + log(spot())\n    n = n + 1\n\n"
                                   "2025-06-03\n    acc = acc + spot()\n    lacc = lacc + log(spot())\n    n = n + 1\n"
                                   "    pays max(acc / n - 100, 0)\n";
        const std::string kControl = "    controlPays max(exp(lacc / n) - 100, 0)\n";

        /// The geometric Asian call on the fixings `t` under Black-Scholes:
        /// ln G is normal, mean ln S0 + (r - q - σ²/2) mean(t), variance
        /// σ² / n² Σ_ij min(t_i, t_j) (Kemna & Vorst 1990, for any dates).
        double geometric_asian_call(const std::vector<Time> &t)
        {
            const double n = static_cast<double>(t.size());
            double mean_t = 0.0, sum_min = 0.0;
            for (Time ti : t)
                mean_t += ti / n;
            for (Time ti : t)
                for (Time tj : t)
                    sum_min += std::min(ti, tj);
            const double mu = std::log(S0) + (r - q - 0.5 * vol * vol) * mean_t;
            const double var = vol * vol * sum_min / (n * n);
            const double sd = std::sqrt(var);
            const double d1 = (mu - std::log(K) + var) / sd;
            const double T = t.back();
            return std::exp(-r * T) * (std::exp(mu + 0.5 * var) * norm_cdf(d1) - K * norm_cdf(d1 - sd));
        }

        ScriptSettings with_mean(double mean)
        {
            ScriptSettings s;
            s.control_mean = mean;
            return s;
        }
    } // namespace

    TEST(AnalyticControls, ParsesAndWritesBack)
    {
        const auto events = scripting::parse_script("2025-06-03\n    controlPays max(spot() - 100, 0)\n    pays 1\n");
        const std::string text = scripting::ScriptWriter::write(events);
        EXPECT_NE(text.find("controlPays max("), std::string::npos) << "[" << text << "]";
        EXPECT_EQ(scripting::ScriptWriter::write(scripting::parse_script(text)), text);
    }

    TEST(AnalyticControls, WithoutAMeanTheControlIsIgnored)
    {
        ScriptedProduct<Real> plain(kAsian, ctx()), declared(kAsian + kControl, ctx());
        Real mean = 0.0;
        EXPECT_FALSE(declared.analytic_control(mean));
        BlackScholesSimModel<Real> m1(S0, r, q, vol), m2(S0, r, q, vol);
        const auto a = simulate<Real>(plain, m1, settings());
        const auto b = simulate<Real>(declared, m2, settings());
        EXPECT_TRUE(same_bits(a.npv(), b.npv())) << a.npv() << " vs " << b.npv();
        EXPECT_TRUE(same_bits(a.std_error(), b.std_error()));
    }

    TEST(AnalyticControls, GeometricControlOfTheArithmeticAsian)
    {
        ScriptedProduct<Real> plain(kAsian, ctx());
        const double geo = geometric_asian_call(plain.timeline());
        ScriptedProduct<Real> controlled(kAsian + kControl, ctx(), with_mean(geo));
        Real mean = 0.0;
        ASSERT_TRUE(controlled.analytic_control(mean));
        EXPECT_EQ(mean, geo);

        BlackScholesSimModel<Real> m1(S0, r, q, vol), m2(S0, r, q, vol);
        const auto a = simulate<Real>(plain, m1, settings());
        const auto b = simulate<Real>(controlled, m2, settings());
        RecordProperty("plain", std::to_string(a.npv()) + " +- " + std::to_string(a.std_error()));
        RecordProperty("controlled", std::to_string(b.npv()) + " +- " + std::to_string(b.std_error()));
        // Unbiased: the same price within the plain estimate's error.
        EXPECT_NEAR(b.npv(), a.npv(), 4.0 * a.std_error());
        // The control removes most of the variance of an Asian.
        EXPECT_LT(b.std_error(), a.std_error() / 5.0) << a.std_error() << " -> " << b.std_error();
        EXPECT_NE(b.diagnostics.find("analytic control"), std::string::npos) << "[" << b.diagnostics << "] " << a.std_error() << " -> " << b.std_error();

        // mc_control_variate = false turns it off: the plain estimate again.
        PricingSettings off = settings();
        off.mc_control_variate = false;
        BlackScholesSimModel<Real> m3(S0, r, q, vol);
        const auto c = simulate<Real>(controlled, m3, off);
        EXPECT_TRUE(same_bits(c.npv(), a.npv()));
    }

    TEST(AnalyticControls, ReachesTheHandWrittenAsianEnginesVariance)
    {
        // The hand-written engine: 252 equally spaced fixings, Sobol, the
        // discrete geometric Asian as its control. The script: the
        // business days of the year, the same control, declared.
        ScriptedProduct<Real> plain(kAsian, ctx());
        const double geo = geometric_asian_call(plain.timeline());
        ScriptedProduct<Real> controlled(kAsian + kControl, ctx(), with_mean(geo));
        BlackScholesSimModel<Real> model(S0, r, q, vol);
        const auto script = simulate<Real>(controlled, model, settings());

        AsianBSInput in{};
        in.spot = S0;
        in.strike = K;
        in.maturity = plain.timeline().back();
        in.rate = r;
        in.dividend = q;
        in.vol = vol;
        in.is_call = true;
        in.n_paths = 1 << 17;
        in.seed = 7;
        const PricingResult engine = default_registry().price(
            {InstrumentKind::EquityAsianOption, ModelKind::BlackScholes, EngineKind::MonteCarlo, PricingInput{in}});

        RecordProperty("script_npv", std::to_string(script.npv()));
        RecordProperty("script_se", std::to_string(script.std_error()));
        RecordProperty("engine_npv", std::to_string(engine.npv));
        RecordProperty("engine_se", std::to_string(engine.mc_std_error));
        // The products differ slightly (the fixing dates), the variances
        // should not: within a factor 1.5 of each other's error.
        EXPECT_LT(script.std_error(), 1.5 * engine.mc_std_error)
            << "script " << script.std_error() << ", engine " << engine.mc_std_error;
        EXPECT_NEAR(script.npv(), engine.npv, 0.02 * engine.npv) << "close products, close prices";
    }

    TEST(AnalyticControls, FuzzyIfsBlendTheControlAndTreeEqualsBytecode)
    {
        const std::string src = "2024-12-03\n    s = spot()\n\n2025-06-03\n"
                                "    if spot() > s then\n        controlPays spot() - s\n        pays 1\n"
                                "    else\n        controlPays 0\n    endIf\n";
        ScriptSettings fuzzy = with_mean(0.0);
        fuzzy.fuzzy = true;
        fuzzy.default_eps = 5.0;
        ScriptSettings tree = fuzzy;
        tree.bytecode = false;
        ScriptedProduct<Real> by_code(src, ctx(), fuzzy), by_tree(src, ctx(), tree);

        BlackScholesSimModel<Real> model(S0, r, q, vol);
        model.init(by_code.timeline(), by_code.defline());
        for (int p = 0; p < 16; ++p)
        {
            Scenario<Real> path;
            allocate_scenario(path, by_code.defline(), 1);
            std::vector<double> z(model.sim_dim());
            for (std::size_t i = 0; i < z.size(); ++i)
                z[i] = 0.3 * std::sin(1.9 * static_cast<double>(i) + 0.7 * p);
            model.generate_path(z, path);
            std::vector<Real> a, b;
            by_tree.payoffs(path, a);
            by_code.payoffs(path, b);
            EXPECT_TRUE(same_bits(a[0], b[0])) << "path " << p;
            EXPECT_TRUE(same_bits(by_tree.control_value(nullptr), by_code.control_value(nullptr))) << "path " << p;
        }
    }

} // namespace quantModeling
