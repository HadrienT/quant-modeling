#include <gtest/gtest.h>

#include "quantModeling/aad/number.hpp"
#include "quantModeling/engines/mc/simulation_engine.hpp"
#include "quantModeling/engines/mc/simulation_engine_aad.hpp"
#include "quantModeling/instruments/simulatable.hpp"
#include "quantModeling/models/equity/merton_jump_diffusion_sim_model.hpp"
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
                using std::max;
                out.assign(1, max(p[0].spots[0] - K, 0.0) / p[0].numeraire);
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

        /// Merton's (1976) own closed form: condition on the number of
        /// jumps, which turns each term into a plain Black-Scholes price at
        /// an effective rate and vol -- derived directly from the same SDE
        /// the model simulates (ln S_T | N ~ Normal with mean
        /// (r-q-lambda*k-sigma^2/2)T + N*jump_mean, variance
        /// sigma^2*T + N*jump_vol^2), not copied from a textbook's own
        /// parametrisation, so it is guaranteed to use the same lambda
        /// convention as the simulator.
        double merton_call(double S, double K, double r, double q, double sigma,
                           double T, double lambda, double jump_mean, double jump_vol)
        {
            const double k = std::exp(jump_mean + 0.5 * jump_vol * jump_vol) - 1.0;
            const double lambda_t = lambda * T;
            double price = 0.0;
            double poisson_pmf = std::exp(-lambda_t); // n = 0 term
            for (int n = 0; n < 30; ++n)
            {
                const double sigma_n = std::sqrt(sigma * sigma + n * jump_vol * jump_vol / T);
                const double r_n = r - lambda * k + n * std::log(1.0 + k) / T;
                price += poisson_pmf * bs_call(S, K, r_n, q, sigma_n, T);
                poisson_pmf *= lambda_t / static_cast<double>(n + 1);
            }
            return price;
        }
    } // namespace

    // ── construction validates its inputs ───────────────────────────────────

    TEST(MertonJumpDiffusionSimModel, RejectsNegativeIntensity)
    {
        EXPECT_THROW((MertonJumpDiffusionSimModel<Real>{100.0, 0.03, 0.0, 0.2,
                                                        -0.1, -0.1, 0.15}),
                     InvalidInput);
    }

    TEST(MertonJumpDiffusionSimModel, RejectsNegativeJumpVol)
    {
        EXPECT_THROW((MertonJumpDiffusionSimModel<Real>{100.0, 0.03, 0.0, 0.2,
                                                        0.5, -0.1, -0.05}),
                     InvalidInput);
    }

    TEST(MertonJumpDiffusionSimModel, ZeroIntensityRecoversBlackScholesExactly)
    {
        const Real S0 = 100, K = 105, r = 0.03, q = 0.01, sigma = 0.2, Tm = 1.0;
        EuroCallT<Real> product(K, Tm);
        MertonJumpDiffusionSimModel<Real> model(S0, r, q, sigma, 0.0, 0.0, 0.0);

        PricingSettings settings;
        settings.mc_paths = 200000;
        settings.mc_seed = 5;
        settings.mc_antithetic = true;
        const SimulationMCResult res = simulate<Real>(product, model, settings);

        EXPECT_NEAR(res.npv(), bs_call(S0, K, r, q, sigma, Tm), 4.0 * res.std_error());
    }

    // ── the closed-form validation: Merton's own series formula, derived
    // from the same SDE the simulator implements ───────────────────────────

    TEST(MertonJumpDiffusionSimModel, MatchesMertonClosedForm)
    {
        const Real S0 = 100, K = 100, r = 0.03, q = 0.0, sigma = 0.15, Tm = 1.0;
        const Real lambda = 0.8, jump_mean = -0.1, jump_vol = 0.2;

        EuroCallT<Real> product(K, Tm);
        MertonJumpDiffusionSimModel<Real> model(S0, r, q, sigma, lambda, jump_mean,
                                                jump_vol);

        PricingSettings settings;
        settings.mc_paths = 400000;
        settings.mc_seed = 11;
        settings.mc_antithetic = true;
        const SimulationMCResult res = simulate<Real>(product, model, settings);

        const double ref = merton_call(S0, K, r, q, sigma, Tm, lambda, jump_mean, jump_vol);
        EXPECT_NEAR(res.npv(), ref, 4.0 * res.std_error());
    }

    // ── the AAD trap, closed ─────────────────────────────────────────────

    TEST(MertonJumpDiffusionSimModel, CloneKeepsItsOwnParameterPointers)
    {
        MertonJumpDiffusionSimModel<Real> model(100.0, 0.03, 0.0, 0.2, 0.5, -0.1, 0.15);
        const auto clone = model.clone();

        ASSERT_EQ(model.parameters().size(), clone->parameters().size());
        for (std::size_t i = 0; i < model.parameters().size(); ++i)
            EXPECT_NE(model.parameters()[i], clone->parameters()[i])
                << "parameter " << i << " (" << model.parameter_labels()[i] << ")";
    }

    // ── jump_mean's adjoint matches a common-random-number bump on the same
    // path: bumping jump_mean/jump_vol never changes which draw decides the
    // jump count (that depends only on lambda), so there is no discreteness
    // to trip over here, unlike a bump on lambda itself could ─────────────

    TEST(MertonJumpDiffusionSimModel, JumpMeanAdjointMatchesCommonRandomNumberBump)
    {
        const Real S0 = 100, r = 0.03, q = 0.0, sigma = 0.15, lambda = 0.8,
                   jump_vol = 0.2, K = 100.0, Tm = 1.0;
        const double base_mean = -0.1;
        const double h = 1e-4;
        const std::vector<double> z = {0.3, 0.9, -0.4}; // diffusion, poisson-uniform, jump-sum

        EuroCallT<Number> product(K, Tm);

        auto price_for = [&](double jm)
        {
            Tape local_tape;
            TapeSwitch guard(local_tape);
            MertonJumpDiffusionSimModel<Number> m{Number(S0), Number(r), Number(q),
                                                  Number(sigma), Number(lambda),
                                                  Number(jm), Number(jump_vol)};
            m.init(product.timeline(), product.defline());
            Scenario<Number> path;
            allocate_scenario(path, product.defline(), m.n_underlyings());
            m.generate_path(std::span<const double>(z), path);
            std::vector<Number> payoffs(1);
            product.payoffs(path, payoffs);
            return payoffs[0].value();
        };

        const double bump = (price_for(base_mean + h) - price_for(base_mean - h)) / (2.0 * h);

        Tape tape;
        TapeSwitch guard(tape);
        Number jm(base_mean);
        MertonJumpDiffusionSimModel<Number> model{Number(S0), Number(r), Number(q),
                                                  Number(sigma), Number(lambda), jm,
                                                  Number(jump_vol)};
        model.init(product.timeline(), product.defline());
        Scenario<Number> path;
        allocate_scenario(path, product.defline(), model.n_underlyings());
        model.generate_path(std::span<const double>(z), path);
        std::vector<Number> payoffs(1);
        product.payoffs(path, payoffs);
        payoffs[0].propagate_to_start();

        EXPECT_NEAR(jm.adjoint(), bump, 1e-5);
    }

    // ── simulate_aad reports every jump parameter, correctly labelled ──────

    TEST(MertonJumpDiffusionSimModel, SimulateAadReportsAllSevenParameters)
    {
        Tape tape;
        TapeSwitch guard(tape);

        EuroCallT<Number> product(100.0, 1.0);
        MertonJumpDiffusionSimModel<Number> model{
            Number(100.0), Number(0.03), Number(0.0), Number(0.15),
            Number(0.5), Number(-0.1), Number(0.2)};

        const AADSimulResults res = simulate_aad(product, model, 50000, 4);

        ASSERT_EQ(res.risk_labels.size(), 7u);
        EXPECT_EQ(res.risk_labels[0], "spot");
        EXPECT_EQ(res.risk_labels[1], "rate");
        EXPECT_EQ(res.risk_labels[2], "div");
        EXPECT_EQ(res.risk_labels[3], "vol");
        EXPECT_EQ(res.risk_labels[4], "jump_intensity");
        EXPECT_EQ(res.risk_labels[5], "jump_mean");
        EXPECT_EQ(res.risk_labels[6], "jump_vol");

        EXPECT_GT(res.risks[0], 0.0); // delta of an ATM call: positive
        for (double se : res.risk_std_errors)
            EXPECT_GT(se, 0.0);
    }

} // namespace quantModeling
