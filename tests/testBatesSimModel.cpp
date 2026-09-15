#include <gtest/gtest.h>

#include "quantModeling/aad/number.hpp"
#include "quantModeling/engines/analytic/heston_cos.hpp"
#include "quantModeling/engines/mc/simulation_engine.hpp"
#include "quantModeling/engines/mc/simulation_engine_aad.hpp"
#include "quantModeling/instruments/simulatable.hpp"
#include "quantModeling/models/equity/bates_sim_model.hpp"
#include "quantModeling/models/equity/heston.hpp"

#include <cmath>
#include <span>
#include <vector>

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

            EuroCallT(Real k, Real maturity) : K(k), tl_{maturity}, dl_(1) {}
            const TimeLine &timeline() const override { return tl_; }
            const std::vector<SampleDef> &defline() const override { return dl_; }
            const std::vector<std::string> &payoff_labels() const override
            {
                return labels_;
            }
            std::size_t n_underlyings() const override { return 1; }
            void payoffs(const Scenario<T> &p, std::vector<T> &out) const override
            {
                using std::max;
                out.assign(1, max(p[0].spots[0] - K, 0.0) / p[0].numeraire);
            }
        };

        /// See testKouJumpDiffusionSimModel.cpp: a general martingale oracle
        /// for a model with no simple closed-form option price of its own.
        template <class T>
        struct DeflatedSpotT final : ISimulatableProduct<T>
        {
            TimeLine tl_;
            std::vector<SampleDef> dl_;
            std::vector<std::string> labels_{"price"};

            explicit DeflatedSpotT(Real maturity) : tl_{maturity}, dl_(1) {}
            const TimeLine &timeline() const override { return tl_; }
            const std::vector<SampleDef> &defline() const override { return dl_; }
            const std::vector<std::string> &payoff_labels() const override
            {
                return labels_;
            }
            std::size_t n_underlyings() const override { return 1; }
            void payoffs(const Scenario<T> &p, std::vector<T> &out) const override
            {
                out.assign(1, p[0].spots[0] / p[0].numeraire);
            }
        };
    } // namespace

    // ── construction validates its inputs ───────────────────────────────────

    TEST(BatesSimModel, RejectsNegativeV0)
    {
        EXPECT_THROW((BatesSimModel<Real>{100.0, 0.03, 0.0, -0.01, 1.5, 0.04,
                                          0.3, -0.6, 0.5, -0.1, 0.15}),
                    InvalidInput);
    }

    TEST(BatesSimModel, RejectsNonPositiveKappa)
    {
        EXPECT_THROW((BatesSimModel<Real>{100.0, 0.03, 0.0, 0.04, 0.0, 0.04,
                                          0.3, -0.6, 0.5, -0.1, 0.15}),
                    InvalidInput);
    }

    TEST(BatesSimModel, RejectsRhoOutOfRange)
    {
        EXPECT_THROW((BatesSimModel<Real>{100.0, 0.03, 0.0, 0.04, 1.5, 0.04,
                                          0.3, -1.5, 0.5, -0.1, 0.15}),
                    InvalidInput);
    }

    TEST(BatesSimModel, RejectsNegativeIntensity)
    {
        EXPECT_THROW((BatesSimModel<Real>{100.0, 0.03, 0.0, 0.04, 1.5, 0.04,
                                          0.3, -0.6, -0.5, -0.1, 0.15}),
                    InvalidInput);
    }

    // ── with lambda = 0, Bates collapses to Heston exactly: cross-check the
    // Monte Carlo simulator against the existing, independently-tested
    // semi-analytic COS pricer ──────────────────────────────────────────────

    TEST(BatesSimModel, ZeroIntensityMatchesHestonCOS)
    {
        const Real S0 = 100, K = 100, r = 0.03, q = 0.0, Tm = 1.0;
        const Real v0 = 0.04, kappa = 1.5, theta = 0.04, xi = 0.3, rho = -0.6;

        EuroCallT<Real> product(K, Tm);
        BatesSimModel<Real> model(S0, r, q, v0, kappa, theta, xi, rho, 0.0,
                                  -0.1, 0.15);

        PricingSettings settings;
        settings.mc_paths = 200000;
        settings.mc_seed = 7;
        settings.mc_antithetic = true;
        const SimulationMCResult res = simulate<Real>(product, model, settings);

        const HestonParams hp{v0, kappa, theta, xi, rho};
        const Real forward = S0 * std::exp((r - q) * Tm);
        const Real df = std::exp(-r * Tm);
        const Real cos_price = heston_cos_price(forward, K, Tm, df, hp, true);

        EXPECT_NEAR(res.npv(), cos_price, 4.0 * res.std_error());
    }

    // ── the general oracle when jumps are on: the compensator must still
    // make the discounted spot a martingale regardless of the stochastic
    // variance process running underneath it ───────────────────────────────

    TEST(BatesSimModel, DiscountedSpotIsAMartingale)
    {
        // With a continuous dividend yield q, the *ex-dividend* stock price
        // deflated by the cash numeraire is not itself a Q-martingale --
        // E[S_T]=S0*exp((r-q)*T) makes E[S_T/numeraire(T)]=S0*exp(-q*T), a
        // dividend-reinvested (total-return) index would be the martingale
        // instead. This is a property of the dividend convention shared by
        // every model in this codebase, not specific to Bates, so the
        // oracle here targets S0*exp(-q*T) rather than S0 itself.
        const Real S0 = 100, r = 0.03, q = 0.01, Tm = 1.0;
        DeflatedSpotT<Real> product(Tm);
        BatesSimModel<Real> model(S0, r, q, 0.04, 1.5, 0.04, 0.3, -0.6, 0.8,
                                  -0.05, 0.1);

        PricingSettings settings;
        settings.mc_paths = 300000;
        settings.mc_seed = 11;
        settings.mc_antithetic = true;
        const SimulationMCResult res = simulate<Real>(product, model, settings);

        const Real expected = S0 * std::exp(-q * Tm);
        EXPECT_NEAR(res.npv(), expected, 4.0 * res.std_error());
    }

    // ── the AAD trap, closed ─────────────────────────────────────────────

    TEST(BatesSimModel, CloneKeepsItsOwnParameterPointers)
    {
        BatesSimModel<Real> model(100.0, 0.03, 0.0, 0.04, 1.5, 0.04, 0.3, -0.6,
                                  0.5, -0.1, 0.15);
        const auto clone = model.clone();

        ASSERT_EQ(model.parameters().size(), clone->parameters().size());
        for (std::size_t i = 0; i < model.parameters().size(); ++i)
            EXPECT_NE(model.parameters()[i], clone->parameters()[i])
                << "parameter " << i << " (" << model.parameter_labels()[i] << ")";
    }

    // ── jump_mean's adjoint matches a common-random-number bump on the same
    // path: bumping jump_mean never changes how many jumps occur (that is
    // decided from the poisson-count uniform alone), so there is no
    // discreteness for the bump to trip over. Tm is pinned to exactly one
    // internal Euler substep (the model's own default max_dt) so the jump
    // term compounds only once rather than across ~dozens of substeps --
    // keeping both sides of the comparison on an O(1) scale instead of one
    // where a handful of ULPs of floating-point noise, compounded 13+
    // times, would need an artificially loose tolerance to absorb. lambda
    // is set high and every gaussian slot fixed at 0.99 so the poisson
    // draw actually produces n_jumps = 1 -- exercising the
    // "n*jump_mean + sqrt(n)*jump_vol*z_jump" sum term itself, not only the
    // deterministic compensator that runs regardless of whether any jump
    // fires ─────────────────────────────────────────────────────────────

    TEST(BatesSimModel, JumpMeanAdjointMatchesCommonRandomNumberBump)
    {
        const Real S0 = 100, r = 0.03, q = 0.0, v0 = 0.04, kappa = 1.5,
                   theta = 0.04, xi = 0.3, rho = -0.4, lambda = 10.0,
                   jump_vol = 0.1, K = 100.0, Tm = 1.0 / 50.0;
        const double base_jump_mean = -0.05;
        const double h = 1e-4;

        EuroCallT<Number> product(K, Tm);

        auto price_for = [&](double jm)
        {
            Tape local_tape;
            TapeSwitch guard(local_tape);
            BatesSimModel<Number> m{Number(S0),    Number(r),     Number(q),
                                    Number(v0),    Number(kappa), Number(theta),
                                    Number(xi),    Number(rho),   Number(lambda),
                                    Number(jm),    Number(jump_vol)};
            m.init(product.timeline(), product.defline());
            Scenario<Number> path;
            allocate_scenario(path, product.defline(), m.n_underlyings());
            std::vector<double> z(m.sim_dim(), 0.99); // fixed CRN, forces n_jumps=1/step
            m.generate_path(std::span<const double>(z), path);
            std::vector<Number> payoffs(1);
            product.payoffs(path, payoffs);
            return payoffs[0].value();
        };

        const double bump =
            (price_for(base_jump_mean + h) - price_for(base_jump_mean - h)) /
            (2.0 * h);

        Tape tape;
        TapeSwitch guard(tape);
        Number jm(base_jump_mean);
        BatesSimModel<Number> model{Number(S0),    Number(r),     Number(q),
                                    Number(v0),    Number(kappa), Number(theta),
                                    Number(xi),    Number(rho),   Number(lambda),
                                    jm,            Number(jump_vol)};
        model.init(product.timeline(), product.defline());
        Scenario<Number> path;
        allocate_scenario(path, product.defline(), model.n_underlyings());
        std::vector<double> z(model.sim_dim(), 0.99);
        model.generate_path(std::span<const double>(z), path);
        std::vector<Number> payoffs(1);
        product.payoffs(path, payoffs);
        payoffs[0].propagate_to_start();

        EXPECT_NEAR(jm.adjoint(), bump, 1e-4);
    }

    // ── simulate_aad reports all eleven parameters, correctly labelled ──────

    TEST(BatesSimModel, SimulateAadReportsAllElevenParameters)
    {
        Tape tape;
        TapeSwitch guard(tape);

        EuroCallT<Number> product(100.0, 1.0);
        BatesSimModel<Number> model{
            Number(100.0), Number(0.03), Number(0.0),  Number(0.04),
            Number(1.5),   Number(0.04), Number(0.3),  Number(-0.6),
            Number(0.5),   Number(-0.1), Number(0.15)};

        const AADSimulResults res = simulate_aad(product, model, 50000, 4);

        ASSERT_EQ(res.risk_labels.size(), 11u);
        EXPECT_EQ(res.risk_labels[0], "spot");
        EXPECT_EQ(res.risk_labels[1], "rate");
        EXPECT_EQ(res.risk_labels[2], "div");
        EXPECT_EQ(res.risk_labels[3], "v0");
        EXPECT_EQ(res.risk_labels[4], "kappa");
        EXPECT_EQ(res.risk_labels[5], "theta");
        EXPECT_EQ(res.risk_labels[6], "xi");
        EXPECT_EQ(res.risk_labels[7], "rho");
        EXPECT_EQ(res.risk_labels[8], "jump_intensity");
        EXPECT_EQ(res.risk_labels[9], "jump_mean");
        EXPECT_EQ(res.risk_labels[10], "jump_vol");

        EXPECT_GT(res.risks[0], 0.0); // delta of an ATM call: positive
        for (double se : res.risk_std_errors)
            EXPECT_GT(se, 0.0);
    }

} // namespace quantModeling
