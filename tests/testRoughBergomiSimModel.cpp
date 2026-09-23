#include <gtest/gtest.h>

#include "quantModeling/aad/number.hpp"
#include "quantModeling/engines/mc/rough_bergomi_hybrid.hpp"
#include "quantModeling/engines/mc/simulation_engine.hpp"
#include "quantModeling/engines/mc/simulation_engine_aad.hpp"
#include "quantModeling/instruments/simulatable.hpp"
#include "quantModeling/models/equity/rough_bergomi_sim_model.hpp"
#include "quantModeling/models/equity/sabr.hpp" // black76_call_price

#include <cmath>
#include <span>
#include <vector>

// issue #37's "vrai différenciateur" (etc/roadmap.md §1c): rough Bergomi in
// the timeline/AAD architecture, ported from the already-tested double-only
// hybrid-scheme engine (engines/mc/rough_bergomi_hybrid.hpp).

namespace quantModeling
{
    namespace
    {
        using aad::Number;
        using aad::Tape;

        struct TapeSwitch
        {
            Tape *saved = Number::tape;
            explicit TapeSwitch(Tape &t) { Number::tape = &t; }
            ~TapeSwitch() { Number::tape = saved; }
        };

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
            const std::vector<std::string> &payoff_labels() const override { return labels_; }
            std::size_t n_underlyings() const override { return 1; }
            void payoffs(const Scenario<T> &p, std::vector<T> &out) const override
            {
                using std::max;
                out.assign(1, max(p[0].spots[0] - K, 0.0) / p[0].numeraire);
            }
        };
    } // namespace

    TEST(RoughBergomiSimModel, RejectsNonPositiveXi0)
    {
        EXPECT_THROW((RoughBergomiSimModel<Real>{100.0, 0.0, 0.0, 0.0, 1.9, -0.7, 0.1}), InvalidInput);
    }

    TEST(RoughBergomiSimModel, RejectsHOutOfRange)
    {
        EXPECT_THROW((RoughBergomiSimModel<Real>{100.0, 0.0, 0.0, 0.04, 1.9, -0.7, 0.6}), InvalidInput);
        EXPECT_THROW((RoughBergomiSimModel<Real>{100.0, 0.0, 0.0, 0.04, 1.9, -0.7, 0.0}), InvalidInput);
    }

    TEST(RoughBergomiSimModel, RejectsMultiDateProducts)
    {
        RoughBergomiSimModel<Real> model(100.0, 0.0, 0.0, 0.04, 1.9, -0.7, 0.1, 50);
        std::vector<SampleDef> defline(2);
        EXPECT_THROW(model.init(TimeLine{0.5, 1.0}, defline), InvalidInput);
    }

    // ── the strongest check: this model must reproduce the same price as
    // the already-tested, independent double-only hybrid-scheme engine, for
    // the same parameters. r = q = 0 makes S directly comparable to that
    // engine's "forward" (no separate discounting/forward-growth step to
    // reconcile) ─────────────────────────────────────────────────────────

    TEST(RoughBergomiSimModel, MatchesTheReferenceHybridEngine)
    {
        const Real s0 = 100.0, xi0 = 0.04, eta = 1.9, rho = -0.7, H = 0.1, Tm = 0.5;
        const std::size_t n_steps = 50;

        EuroCallT<Real> product(100.0, Tm);
        RoughBergomiSimModel<Real> model(s0, 0.0, 0.0, xi0, eta, rho, H, n_steps);

        PricingSettings ps;
        ps.mc_paths = 80000;
        ps.mc_seed = 3;
        ps.mc_antithetic = true;
        const SimulationMCResult res = simulate<Real>(product, model, ps);

        RoughBergomiParams p;
        p.H = H;
        p.eta = eta;
        p.rho = rho;
        RoughBergomiSettings settings;
        settings.n_steps = n_steps;
        settings.n_paths = 80000;
        settings.seed = 3;
        const RoughBergomiResult ref = rough_bergomi_price(
            s0, 100.0, Tm, 1.0, p, [xi0](Real)
            { return xi0; },
            true, settings);

        EXPECT_NEAR(res.npv(), ref.price, 4.0 * (res.std_error() + ref.std_error));
    }

    // ── the same degenerate-case check the reference engine's own test
    // suite uses: eta -> 0 collapses V_t to the deterministic xi0, so the
    // model must reduce to plain Black-Scholes regardless of H or rho ─────

    TEST(RoughBergomiSimModel, ReducesToBlackScholesWhenEtaVanishes)
    {
        const Real s0 = 100.0, sigma = 0.25, Tm = 0.5;
        RoughBergomiSimModel<Real> model(s0, 0.0, 0.0, sigma * sigma, 1e-4, -0.6, 0.1, 50);

        for (Real K : {80.0, 100.0, 120.0})
        {
            EuroCallT<Real> product(K, Tm);
            PricingSettings ps;
            ps.mc_paths = 60000;
            ps.mc_seed = 42;
            ps.mc_antithetic = true;
            const SimulationMCResult res = simulate<Real>(product, model, ps);

            const Real bs = black76_call_price(s0, K, Tm, sigma, 1.0);
            EXPECT_NEAR(res.npv(), bs, 4.0 * res.std_error()) << "strike " << K;
        }
    }

    TEST(RoughBergomiSimModel, CloneKeepsItsOwnParameterPointers)
    {
        RoughBergomiSimModel<Real> model(100.0, 0.03, 0.01, 0.04, 1.9, -0.7, 0.1, 50);
        const auto clone = model.clone();

        ASSERT_EQ(model.parameters().size(), clone->parameters().size());
        for (std::size_t i = 0; i < model.parameters().size(); ++i)
            EXPECT_NE(model.parameters()[i], clone->parameters()[i])
                << "parameter " << i << " (" << model.parameter_labels()[i] << ")";
    }

    TEST(RoughBergomiSimModel, SimulateAadReportsAllSixParameters)
    {
        Tape tape;
        TapeSwitch guard(tape);

        EuroCallT<Number> product(100.0, 0.5);
        RoughBergomiSimModel<Number> model{Number(100.0), Number(0.03), Number(0.01),
                                           Number(0.04), Number(1.9), Number(-0.7),
                                           0.1, 40};

        const AADSimulResults res = simulate_aad(product, model, 30000, 5);

        ASSERT_EQ(res.risk_labels.size(), 6u);
        EXPECT_EQ(res.risk_labels[0], "spot");
        EXPECT_EQ(res.risk_labels[3], "xi0");
        EXPECT_EQ(res.risk_labels[4], "eta");
        EXPECT_EQ(res.risk_labels[5], "rho");

        EXPECT_TRUE(std::isfinite(res.price));
        for (std::size_t i = 0; i < res.risks.size(); ++i)
        {
            EXPECT_TRUE(std::isfinite(res.risks[i])) << res.risk_labels[i];
            EXPECT_GT(res.risk_std_errors[i], 0.0) << res.risk_labels[i];
        }
        EXPECT_GT(res.risks[0], 0.0); // delta of an ATM call: positive
    }

} // namespace quantModeling
