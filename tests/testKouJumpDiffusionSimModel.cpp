#include <gtest/gtest.h>

#include "quantModeling/aad/number.hpp"
#include "quantModeling/engines/mc/simulation_engine.hpp"
#include "quantModeling/engines/mc/simulation_engine_aad.hpp"
#include "quantModeling/instruments/simulatable.hpp"
#include "quantModeling/models/equity/kou_jump_diffusion_sim_model.hpp"
#include "quantModeling/utils/stats.hpp"

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

        /// The deflated spot itself: E^Q[S_T / numeraire(T)] = S_0 always,
        /// for any arbitrage-free model regardless of its own dynamics --
        /// exactly the identity a jump compensator has to get right. A
        /// general oracle for models with no simple closed-form option
        /// price, like Kou's own Hh-function series.
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

        double bs_call(double S, double K, double r, double q, double v, double T)
        {
            const double sd = v * std::sqrt(T);
            const double d1 = (std::log(S / K) + (r - q + 0.5 * v * v) * T) / sd;
            const double d2 = d1 - sd;
            return S * std::exp(-q * T) * norm_cdf(d1) -
                   K * std::exp(-r * T) * norm_cdf(d2);
        }
    } // namespace

    // ── construction validates its inputs ───────────────────────────────────

    TEST(KouJumpDiffusionSimModel, RejectsNegativeIntensity)
    {
        EXPECT_THROW((KouJumpDiffusionSimModel<Real>{100.0, 0.03, 0.0, 0.2, -0.1,
                                                     0.4, 3.0, 4.0}),
                    InvalidInput);
    }

    TEST(KouJumpDiffusionSimModel, RejectsProbabilityOutOfRange)
    {
        EXPECT_THROW((KouJumpDiffusionSimModel<Real>{100.0, 0.03, 0.0, 0.2, 0.5,
                                                     1.5, 3.0, 4.0}),
                    InvalidInput);
    }

    TEST(KouJumpDiffusionSimModel, RejectsEtaUpAtOrBelowOne)
    {
        EXPECT_THROW((KouJumpDiffusionSimModel<Real>{100.0, 0.03, 0.0, 0.2, 0.5,
                                                     0.4, 1.0, 4.0}),
                    InvalidInput);
    }

    TEST(KouJumpDiffusionSimModel, RejectsNonPositiveEtaDown)
    {
        EXPECT_THROW((KouJumpDiffusionSimModel<Real>{100.0, 0.03, 0.0, 0.2, 0.5,
                                                     0.4, 3.0, 0.0}),
                    InvalidInput);
    }

    TEST(KouJumpDiffusionSimModel, ZeroIntensityRecoversBlackScholesExactly)
    {
        const Real S0 = 100, K = 105, r = 0.03, q = 0.01, sigma = 0.2, Tm = 1.0;
        EuroCallT<Real> product(K, Tm);
        KouJumpDiffusionSimModel<Real> model(S0, r, q, sigma, 0.0, 0.4, 3.0, 4.0);

        PricingSettings settings;
        settings.mc_paths = 200000;
        settings.mc_seed = 5;
        settings.mc_antithetic = true;
        const SimulationMCResult res = simulate<Real>(product, model, settings);

        EXPECT_NEAR(res.npv(), bs_call(S0, K, r, q, sigma, Tm), 4.0 * res.std_error());
    }

    // ── the general oracle: no closed-form call price is reproduced from
    // scratch here (Kou's own is a messy Hh-function series not worth the
    // transcription risk), but the compensator k must make the discounted
    // spot a martingale regardless of the jump-size distribution's shape ──

    TEST(KouJumpDiffusionSimModel, DiscountedSpotIsAMartingale)
    {
        // With a continuous dividend yield q, the *ex-dividend* stock price
        // deflated by the cash numeraire is not itself a Q-martingale --
        // E[S_T]=S0*exp((r-q)*T) makes E[S_T/numeraire(T)]=S0*exp(-q*T), a
        // dividend-reinvested (total-return) index would be the martingale
        // instead. Confirmed by direct high-precision simulation that this
        // test's original target of S0 only "passed" at 300k paths because
        // Kou's heavy jump tail inflated std_error enough to mask a
        // genuine, deterministic ~1% bias -- at 3M paths the same model
        // converges cleanly to S0*exp(-q*T), not S0.
        const Real S0 = 100, r = 0.03, q = 0.01, sigma = 0.2, Tm = 1.0;
        DeflatedSpotT<Real> product(Tm);
        KouJumpDiffusionSimModel<Real> model(S0, r, q, sigma, 1.2, 0.3, 2.0, 3.0);

        PricingSettings settings;
        settings.mc_paths = 300000;
        settings.mc_seed = 9;
        settings.mc_antithetic = true;
        const SimulationMCResult res = simulate<Real>(product, model, settings);

        const Real expected = S0 * std::exp(-q * Tm);
        EXPECT_NEAR(res.npv(), expected, 4.0 * res.std_error());
    }

    // ── the AAD trap, closed ─────────────────────────────────────────────

    TEST(KouJumpDiffusionSimModel, CloneKeepsItsOwnParameterPointers)
    {
        KouJumpDiffusionSimModel<Real> model(100.0, 0.03, 0.0, 0.2, 0.5, 0.4, 3.0, 4.0);
        const auto clone = model.clone();

        ASSERT_EQ(model.parameters().size(), clone->parameters().size());
        for (std::size_t i = 0; i < model.parameters().size(); ++i)
            EXPECT_NE(model.parameters()[i], clone->parameters()[i])
                << "parameter " << i << " (" << model.parameter_labels()[i] << ")";
    }

    // ── eta_down's adjoint matches a common-random-number bump on the same
    // path: bumping eta1/eta2 never changes which branch (up or down) a
    // jump lands on -- only p does -- so there is no discreteness to trip
    // over here ──────────────────────────────────────────────────────────

    TEST(KouJumpDiffusionSimModel, EtaDownAdjointMatchesCommonRandomNumberBump)
    {
        const Real S0 = 100, r = 0.03, q = 0.0, sigma = 0.15, lambda = 0.8,
                   p = 0.35, eta1 = 3.0, K = 100.0, Tm = 1.0;
        const double base_eta2 = 4.0;
        const double h = 1e-4;
        // diffusion, poisson-uniform, then max_jumps_per_step (default 10)
        // jump-slot draws
        std::vector<double> z = {0.3, 0.95};
        z.resize(2 + 10, -1.5); // a handful of "down" draws so >=1 jump lands and is exercised

        EuroCallT<Number> product(K, Tm);

        auto price_for = [&](double e2)
        {
            Tape local_tape;
            TapeSwitch guard(local_tape);
            KouJumpDiffusionSimModel<Number> m{Number(S0),   Number(r),   Number(q),
                                               Number(sigma), Number(lambda),
                                               Number(p),     Number(eta1), Number(e2)};
            m.init(product.timeline(), product.defline());
            Scenario<Number> path;
            allocate_scenario(path, product.defline(), m.n_underlyings());
            m.generate_path(std::span<const double>(z), path);
            std::vector<Number> payoffs(1);
            product.payoffs(path, payoffs);
            return payoffs[0].value();
        };

        const double bump = (price_for(base_eta2 + h) - price_for(base_eta2 - h)) / (2.0 * h);

        Tape tape;
        TapeSwitch guard(tape);
        Number e2(base_eta2);
        KouJumpDiffusionSimModel<Number> model{Number(S0),   Number(r),   Number(q),
                                               Number(sigma), Number(lambda),
                                               Number(p),     Number(eta1), e2};
        model.init(product.timeline(), product.defline());
        Scenario<Number> path;
        allocate_scenario(path, product.defline(), model.n_underlyings());
        model.generate_path(std::span<const double>(z), path);
        std::vector<Number> payoffs(1);
        product.payoffs(path, payoffs);
        payoffs[0].propagate_to_start();

        EXPECT_NEAR(e2.adjoint(), bump, 1e-5);
    }

    // ── simulate_aad reports all eight parameters, correctly labelled ──────

    TEST(KouJumpDiffusionSimModel, SimulateAadReportsAllEightParameters)
    {
        Tape tape;
        TapeSwitch guard(tape);

        EuroCallT<Number> product(100.0, 1.0);
        KouJumpDiffusionSimModel<Number> model{
            Number(100.0), Number(0.03), Number(0.0), Number(0.15),
            Number(0.5),   Number(0.4),  Number(3.0),  Number(4.0)};

        const AADSimulResults res = simulate_aad(product, model, 50000, 4);

        ASSERT_EQ(res.risk_labels.size(), 8u);
        EXPECT_EQ(res.risk_labels[0], "spot");
        EXPECT_EQ(res.risk_labels[1], "rate");
        EXPECT_EQ(res.risk_labels[2], "div");
        EXPECT_EQ(res.risk_labels[3], "vol");
        EXPECT_EQ(res.risk_labels[4], "jump_intensity");
        EXPECT_EQ(res.risk_labels[5], "jump_prob_up");
        EXPECT_EQ(res.risk_labels[6], "jump_eta_up");
        EXPECT_EQ(res.risk_labels[7], "jump_eta_down");

        EXPECT_GT(res.risks[0], 0.0); // delta of an ATM call: positive
        for (double se : res.risk_std_errors)
            EXPECT_GT(se, 0.0);
    }

} // namespace quantModeling
