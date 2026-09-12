#include <gtest/gtest.h>

#include "quantModeling/aad/number.hpp"
#include "quantModeling/engines/mc/simulation_engine_aad.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/instruments/simulatable.hpp"
#include "quantModeling/market/valuation_context.hpp"
#include "quantModeling/models/equity/bs_sim_model.hpp"
#include "quantModeling/models/equity/multi_asset_bs_sim_model.hpp"
#include "quantModeling/utils/stats.hpp"

#include <Eigen/Core>
#include <cmath>
#include <span>
#include <stdexcept>
#include <vector>

namespace quantModeling
{
    namespace
    {
        using aad::Number;
        using aad::Tape;

        /// Points Number::tape at `t` for the scope, restoring whatever it
        /// was on destruction (see tests/testAADTape.cpp for why this is
        /// needed every time a test wants an isolated tape).
        struct TapeSwitch
        {
            Tape *saved = Number::tape;
            explicit TapeSwitch(Tape &t) { Number::tape = &t; }
            ~TapeSwitch() { Number::tape = saved; }
        };

        /// A European call, templated on T so it can be priced under
        /// T = Real (the ordinary engine) or T = Number (simulate_aad) --
        /// the same product code either way, per the timeline architecture.
        template <class T>
        struct EuroCallT final : ISimulatableProduct<T>
        {
            Real K;
            TimeLine tl_;
            std::vector<SampleDef> dl_;
            std::vector<std::string> labels_{"price"};

            EuroCallT(Real k, Real maturity)
                : K(k), tl_{maturity}, dl_(1) {}
            const TimeLine &timeline() const override { return tl_; }
            const std::vector<SampleDef> &defline() const override { return dl_; }
            const std::vector<std::string> &payoff_labels() const override
            {
                return labels_;
            }
            std::size_t n_underlyings() const override { return 1; }
            void payoffs(const Scenario<T> &p, std::vector<T> &out) const override
            {
                using std::max; // ADL: aad::max for T = Number, std::max otherwise
                out.assign(1, max(p[0].spots[0] - K, 0.0) / p[0].numeraire);
            }
        };
    } // namespace

    // ── Black-Scholes: delta, rho, div and vega adjoints match the analytic
    // greeks, computed by simulate_aad in a single run ─────────────────────

    TEST(AADSimulation, BlackScholesGreeksMatchAnalytic)
    {
        const Real S0 = 100, r = 0.03, q = 0.01, sigma = 0.2, K = 105, Tm = 1.5;

        Tape tape;
        TapeSwitch guard(tape);

        EuroCallT<Number> product(K, Tm);
        // Brace-init: BlackScholesSimModel<Number> model(Number(S0), ...)
        // parses as a function *declaration* named model (a vexing parse --
        // Number(S0) is syntactically also "a parameter named S0").
        BlackScholesSimModel<Number> model{Number(S0), Number(r), Number(q),
                                           Number(sigma)};

        const AADSimulResults res = simulate_aad(product, model, 200000, 7);

        const double sd = sigma * std::sqrt(Tm);
        const double d1 = (std::log(S0 / K) + (r - q + 0.5 * sigma * sigma) * Tm) / sd;
        const double d2 = d1 - sd;
        const double ref_price =
            S0 * std::exp(-q * Tm) * norm_cdf(d1) - K * std::exp(-r * Tm) * norm_cdf(d2);
        const double ref_delta = std::exp(-q * Tm) * norm_cdf(d1);
        const double ref_rho = K * Tm * std::exp(-r * Tm) * norm_cdf(d2);
        const double ref_div = -Tm * S0 * std::exp(-q * Tm) * norm_cdf(d1);
        const double ref_vega = S0 * std::exp(-q * Tm) * norm_pdf(d1) * std::sqrt(Tm);

        EXPECT_NEAR(res.price, ref_price, 4.0 * res.price_std_error);

        ASSERT_EQ(res.risk_labels.size(), 4u);
        EXPECT_EQ(res.risk_labels[0], "spot");
        EXPECT_EQ(res.risk_labels[1], "rate");
        EXPECT_EQ(res.risk_labels[2], "div");
        EXPECT_EQ(res.risk_labels[3], "vol");

        EXPECT_NEAR(res.risks[0], ref_delta, 4.0 * res.risk_std_errors[0]);
        EXPECT_NEAR(res.risks[1], ref_rho, 4.0 * res.risk_std_errors[1]);
        EXPECT_NEAR(res.risks[2], ref_div, 4.0 * res.risk_std_errors[2]);
        EXPECT_NEAR(res.risks[3], ref_vega, 4.0 * res.risk_std_errors[3]);
        EXPECT_GT(res.risk_std_errors[0], 0.0);
    }

    // ── the same property, but through ScriptedProduct<Number> instead of a
    // hand-written C++ payoff: WP16's Evaluator<T> was already templated on
    // T with no AAD-specific work (every math call inside it is already
    // unqualified, `using std::exp; return exp(a);` -- ADL-safe by the same
    // convention as bs_sim_model.hpp), so the blueprint's promised "the
    // scripting lot benefits for free" is something to actually verify, not
    // just trust because the types happen to line up and it compiles: a
    // template instantiating cleanly is exactly the silent trap (§0) if
    // nothing on the path from spot() to pays actually touches a Number.
    TEST(AADSimulation, ScriptedEuropeanCallGreeksMatchAnalytic)
    {
        const Real S0 = 100, r = 0.03, q = 0.01, sigma = 0.2, K = 105;

        Tape tape;
        TapeSwitch guard(tape);

        const ValuationContext ctx{Date::today()};
        ScriptedProduct<Number> product(
            "2029-01-01\n    pays max(spot() - 105, 0)\n", ctx);
        BlackScholesSimModel<Number> model{Number(S0), Number(r), Number(q),
                                           Number(sigma)};

        const AADSimulResults res = simulate_aad(product, model, 200000, 7);
        const double Tm = product.timeline().front(); // whatever "2029-01-01" resolves to from today

        const double sd = sigma * std::sqrt(Tm);
        const double d1 = (std::log(S0 / K) + (r - q + 0.5 * sigma * sigma) * Tm) / sd;
        const double d2 = d1 - sd;
        const double ref_price =
            S0 * std::exp(-q * Tm) * norm_cdf(d1) - K * std::exp(-r * Tm) * norm_cdf(d2);
        const double ref_delta = std::exp(-q * Tm) * norm_cdf(d1);
        const double ref_vega = S0 * std::exp(-q * Tm) * norm_pdf(d1) * std::sqrt(Tm);

        EXPECT_NEAR(res.price, ref_price, 4.0 * res.price_std_error);
        ASSERT_EQ(res.risk_labels.size(), 4u);
        EXPECT_NEAR(res.risks[0], ref_delta, 4.0 * res.risk_std_errors[0]); // spot
        EXPECT_NEAR(res.risks[3], ref_vega, 4.0 * res.risk_std_errors[3]); // vol
    }

    // ── the same property, one step further: a scripted payoff that actually
    // needs more than one asset (spot(0), spot(1)) against
    // MultiAssetBSSimModel<Number> -- the piece that used to make "the
    // scripting language only ever sees Black-Scholes" literally true ──────

    TEST(AADSimulation, ScriptedWorstOfTwoAssetsNeedsTwoUnderlyings)
    {
        const ValuationContext ctx{Date::today()};
        ScriptedProduct<Number> product(
            "2029-01-01\n    a = spot(0) / 100\n    b = spot(1) / 110\n"
            "    pays 1000 * min(a, b)\n",
            ctx);
        EXPECT_EQ(product.n_underlyings(), 2u);
    }

    TEST(AADSimulation, ScriptedWorstOfDeltaMatchesCommonRandomNumberBump)
    {
        const Real r = 0.03, sigma0 = 0.2, sigma1 = 0.25;
        const std::vector<double> z = {0.4, -0.6};
        const double h = 1e-4;

        Eigen::MatrixXd corr(2, 2);
        corr << 1.0, 0.3, 0.3, 1.0;

        const ValuationContext ctx{Date::today()};
        ScriptedProduct<Number> product(
            "2029-01-01\n    a = spot(0) / 100\n    b = spot(1) / 110\n"
            "    pays 1000 * min(a, b)\n",
            ctx);

        auto price_for_spot0 = [&](double s0_value)
        {
            Tape local_tape;
            TapeSwitch local_guard(local_tape);
            MultiAssetBSSimModel<Number> m{
                {Number(s0_value), Number(100.0)},
                Number(r),
                {Number(0.0), Number(0.0)},
                {Number(sigma0), Number(sigma1)},
                corr};
            m.init(product.timeline(), product.defline());
            Scenario<Number> path;
            allocate_scenario(path, product.defline(), m.n_underlyings());
            m.generate_path(std::span<const double>(z), path);
            std::vector<Number> payoffs(1);
            product.payoffs(path, payoffs);
            return payoffs[0].value();
        };

        const double bump_delta =
            (price_for_spot0(100.0 + h) - price_for_spot0(100.0 - h)) / (2.0 * h);

        Tape tape;
        TapeSwitch guard(tape);
        Number s0_0(100.0);
        MultiAssetBSSimModel<Number> model{
            {s0_0, Number(100.0)},
            Number(r),
            {Number(0.0), Number(0.0)},
            {Number(sigma0), Number(sigma1)},
            corr};
        model.init(product.timeline(), product.defline());
        Scenario<Number> path;
        allocate_scenario(path, product.defline(), model.n_underlyings());
        model.generate_path(std::span<const double>(z), path);
        std::vector<Number> payoffs(1);
        product.payoffs(path, payoffs);
        payoffs[0].propagate_to_start();

        EXPECT_NEAR(s0_0.adjoint(), bump_delta, 1e-6);
    }

    TEST(AADSimulation, ScriptedWorstOfSimulateAadGivesSensibleRisks)
    {
        Tape tape;
        TapeSwitch guard(tape);

        const ValuationContext ctx{Date::today()};
        ScriptedProduct<Number> product(
            "2029-01-01\n    a = spot(0) / 100\n    b = spot(1) / 100\n"
            "    pays 1000 * min(a, b)\n",
            ctx);

        Eigen::MatrixXd corr(2, 2);
        corr << 1.0, 0.3, 0.3, 1.0;
        MultiAssetBSSimModel<Number> model{
            {Number(100.0), Number(100.0)},
            Number(0.03),
            {Number(0.0), Number(0.0)},
            {Number(0.2), Number(0.2)},
            corr};

        const AADSimulResults res = simulate_aad(product, model, 100000, 21);

        // rate, spot[0], spot[1], div[0], div[1], vol[0], vol[1]
        ASSERT_EQ(res.risk_labels.size(), 7u);
        EXPECT_EQ(res.risk_labels[1], "spot[0]");
        EXPECT_EQ(res.risk_labels[2], "spot[1]");
        EXPECT_GT(res.risks[1], 0.0); // worst-of is increasing in each spot
        EXPECT_GT(res.risks[2], 0.0);
    }

    // ── pathwise: the adjoint delta of ONE path equals a bump on that SAME
    // path (common random numbers cancel the Monte-Carlo noise a multi-path
    // comparison would otherwise need to average away), to 1e-6 ───────────

    TEST(AADSimulation, PathwiseAdjointMatchesCommonRandomNumberBump)
    {
        const Real r = 0.03, q = 0.01, sigma = 0.2, K = 105.0, Tm = 1.0;
        const double z = 0.37; // one fixed gaussian draw, shared by every evaluation
        const double h = 1e-4;

        EuroCallT<Number> product(K, Tm);

        auto price_for_spot = [&](double s0_value)
        {
            Tape local_tape;
            TapeSwitch local_guard(local_tape);
            BlackScholesSimModel<Number> m{Number(s0_value), Number(r), Number(q),
                                           Number(sigma)};
            m.init(product.timeline(), product.defline());
            Scenario<Number> path;
            allocate_scenario(path, product.defline(), m.n_underlyings());
            m.generate_path(std::span<const double>(&z, 1), path);
            std::vector<Number> payoffs(1);
            product.payoffs(path, payoffs);
            return payoffs[0].value();
        };

        const double bump_delta =
            (price_for_spot(100.0 + h) - price_for_spot(100.0 - h)) / (2.0 * h);

        Tape tape;
        TapeSwitch guard(tape);
        Number s0(100.0);
        BlackScholesSimModel<Number> model(s0, Number(r), Number(q), Number(sigma));
        model.init(product.timeline(), product.defline());
        Scenario<Number> path;
        allocate_scenario(path, product.defline(), model.n_underlyings());
        model.generate_path(std::span<const double>(&z, 1), path);
        std::vector<Number> payoffs(1);
        product.payoffs(path, payoffs);
        payoffs[0].propagate_to_start();

        EXPECT_NEAR(s0.adjoint(), bump_delta, 1e-6);
    }

    TEST(AADSimulation, RejectsZeroPaths)
    {
        Tape tape;
        TapeSwitch guard(tape);
        EuroCallT<Number> product(100.0, 1.0);
        BlackScholesSimModel<Number> model(Number(100.0), Number(0.03),
                                           Number(0.0), Number(0.2));
        EXPECT_THROW(simulate_aad(product, model, 0), InvalidInput);
    }

    // ── simulate_aad's own tape.rewind() is immediately followed by the RAII
    // guard that clears it -- so a run that throws partway through (a model
    // bug, an unexpectedly large product) does not leave whatever it had
    // recorded up to that point parked on the calling thread's tape
    // indefinitely. Testing the actual production guard type directly,
    // rather than reproducing a fresh copy of it here, or contriving a
    // product large enough to overrun the default 256 MB ceiling just to
    // observe the same thing indirectly.
    TEST(AADSimulation, TapeClearGuardRunsEvenWhenAnExceptionUnwindsThroughIt)
    {
        Tape tape;
        TapeSwitch guard(tape);
        Number leaf(1.0); // put something on the tape first
        (void)leaf;

        const std::size_t baseline = Tape().block_count();
        bool threw = false;
        try
        {
            const detail::TapeClearGuard clear_on_exit{tape};
            throw std::runtime_error("simulated mid-run failure");
        }
        catch (const std::runtime_error &)
        {
            threw = true;
        }

        EXPECT_TRUE(threw);
        EXPECT_EQ(tape.block_count(), baseline);
    }

    // ── to_pricing_result(): every risk lands in RiskReport regardless of
    // its name, and the ones matching BlackScholesSimModel's own labels
    // additionally fill the classic Greeks slots (blueprint §13.1) ────────

    TEST(AADSimulation, ToPricingResultFillsRiskReportAndMatchingGreeks)
    {
        AADSimulResults aad_res;
        aad_res.price = 12.3;
        aad_res.price_std_error = 0.05;
        aad_res.risk_labels = {"spot", "rate", "div", "vol"};
        aad_res.risks = {0.5, 90.0, -120.0, 55.0};
        aad_res.risk_std_errors = {0.001, 0.2, 0.3, 0.25};
        aad_res.diagnostics = "test";

        const PricingResult res = to_pricing_result(aad_res);

        EXPECT_DOUBLE_EQ(res.npv, 12.3);
        EXPECT_DOUBLE_EQ(res.mc_std_error, 0.05);

        ASSERT_TRUE(res.risks.has_value());
        EXPECT_EQ(res.risks->labels, aad_res.risk_labels);
        EXPECT_EQ(res.risks->values, aad_res.risks);
        EXPECT_EQ(res.risks->std_errors, aad_res.risk_std_errors);

        ASSERT_TRUE(res.greeks.delta.has_value());
        EXPECT_DOUBLE_EQ(*res.greeks.delta, 0.5);
        ASSERT_TRUE(res.greeks.rho.has_value());
        EXPECT_DOUBLE_EQ(*res.greeks.rho, 90.0);
        ASSERT_TRUE(res.greeks.vega.has_value());
        EXPECT_DOUBLE_EQ(*res.greeks.vega, 55.0);

        // "div" has no matching Greeks slot: correctly absent there, but
        // still present in risks above -- the whole point of RiskReport.
        EXPECT_FALSE(res.greeks.gamma.has_value());
        EXPECT_FALSE(res.greeks.theta.has_value());
    }

} // namespace quantModeling
