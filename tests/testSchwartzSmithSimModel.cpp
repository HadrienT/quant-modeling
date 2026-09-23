#include <gtest/gtest.h>

#include "quantModeling/aad/number.hpp"
#include "quantModeling/engines/mc/simulation_engine.hpp"
#include "quantModeling/engines/mc/simulation_engine_aad.hpp"
#include "quantModeling/instruments/simulatable.hpp"
#include "quantModeling/models/commodity/schwartz_smith_sim_model.hpp"

#include <cmath>
#include <span>
#include <vector>

// issue #37's commodities item: Schwartz-Smith two-factor mean reversion.

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

        /// log(S_T) and its square, undeflated (raw path statistics, not a
        /// discounted payoff) -- lets a single simulate<Real>() call report
        /// both E[ln S_T] and Var[ln S_T] = E[(ln S_T)^2] - E[ln S_T]^2 with
        /// their own Monte-Carlo standard errors.
        template <class T>
        struct LogSpotMomentsT final : ISimulatableProduct<T>
        {
            TimeLine tl_;
            std::vector<SampleDef> dl_;
            std::vector<std::string> labels_{"logS", "logS2"};

            explicit LogSpotMomentsT(Real maturity)
                : tl_{maturity}, dl_(1) {}
            const TimeLine &timeline() const override { return tl_; }
            const std::vector<SampleDef> &defline() const override { return dl_; }
            const std::vector<std::string> &payoff_labels() const override { return labels_; }
            std::size_t n_underlyings() const override { return 1; }
            void payoffs(const Scenario<T> &p, std::vector<T> &out) const override
            {
                using std::log;
                const T ls = log(p[0].spots[0]);
                out.assign(2, T(0.0));
                out[0] = ls;
                out[1] = ls * ls;
            }
        };

        template <class T>
        struct LogSpotT final : ISimulatableProduct<T>
        {
            TimeLine tl_;
            std::vector<SampleDef> dl_;
            std::vector<std::string> labels_{"logS"};

            explicit LogSpotT(Real maturity)
                : tl_{maturity}, dl_(1) {}
            const TimeLine &timeline() const override { return tl_; }
            const std::vector<SampleDef> &defline() const override { return dl_; }
            const std::vector<std::string> &payoff_labels() const override { return labels_; }
            std::size_t n_underlyings() const override { return 1; }
            void payoffs(const Scenario<T> &p, std::vector<T> &out) const override
            {
                using std::log;
                out.assign(1, log(p[0].spots[0]));
            }
        };
    } // namespace

    TEST(SchwartzSmithSimModel, RejectsNonPositiveSpot)
    {
        EXPECT_THROW((SchwartzSmithSimModel<Real>{0.0, 0.03, 0.0, 1.2, 0.3, 0.0, 0.2, -0.3}),
                     InvalidInput);
    }

    TEST(SchwartzSmithSimModel, RejectsNonPositiveKappa)
    {
        EXPECT_THROW((SchwartzSmithSimModel<Real>{50.0, 0.03, 0.0, 0.0, 0.3, 0.0, 0.2, -0.3}),
                     InvalidInput);
    }

    TEST(SchwartzSmithSimModel, RejectsRhoOutOfRange)
    {
        EXPECT_THROW((SchwartzSmithSimModel<Real>{50.0, 0.03, 0.0, 1.2, 0.3, 0.0, 0.2, -1.5}),
                     InvalidInput);
    }

    // ── the exact joint (chi, xi) simulation's own promise, checked
    // directly: the mean and variance of ln(S_T) must match the closed-form
    // Gaussian moments -- the same kind of check
    // VasicekSimModel.DiscountedUnitPayoffMatchesZcbPrice performs for its
    // own exact joint draw ──────────────────────────────────────────────────

    TEST(SchwartzSmithSimModel, LogSpotMeanAndVarianceMatchClosedForm)
    {
        const Real s0 = 50.0, r = 0.03, chi0 = 0.1, kappa = 1.2, sigma_chi = 0.35,
                   mu_xi = 0.02, sigma_xi = 0.18, rho = -0.3, Tm = 1.5;

        LogSpotMomentsT<Real> product(Tm);
        SchwartzSmithSimModel<Real> model(s0, r, chi0, kappa, sigma_chi, mu_xi, sigma_xi, rho);

        PricingSettings ps;
        ps.mc_paths = 200000;
        ps.mc_seed = 9;
        ps.mc_antithetic = true;
        const SimulationMCResult res = simulate<Real>(product, model, ps);

        const Real mc_mean = res.values[0];
        const Real mc_var = res.values[1] - mc_mean * mc_mean;

        const Real xi0 = std::log(s0) - chi0;
        const Real exp_kT = std::exp(-kappa * Tm);
        const Real expected_mean = chi0 * exp_kT + xi0 + mu_xi * Tm;

        const Real var_chi = sigma_chi * sigma_chi * (1.0 - exp_kT * exp_kT) / (2.0 * kappa);
        const Real var_xi = sigma_xi * sigma_xi * Tm;
        const Real cov = rho * sigma_chi * sigma_xi * (1.0 - exp_kT) / kappa;
        const Real expected_var = var_chi + var_xi + 2.0 * cov;

        EXPECT_NEAR(mc_mean, expected_mean, 4.0 * res.std_errors[0]);
        // Var[X]'s own Monte-Carlo standard error isn't res.std_errors[1]
        // (that one is for E[X^2]) -- a modest relative-plus-absolute
        // tolerance stands in, the same style SABRSimModel's own
        // closed-form cross-check test uses.
        EXPECT_NEAR(mc_var, expected_var, 0.05 * expected_var + 0.01);
    }

    TEST(SchwartzSmithSimModel, CloneKeepsItsOwnParameterPointers)
    {
        SchwartzSmithSimModel<Real> model(50.0, 0.03, 0.0, 1.2, 0.3, 0.0, 0.2, -0.3);
        const auto clone = model.clone();

        ASSERT_EQ(model.parameters().size(), clone->parameters().size());
        for (std::size_t i = 0; i < model.parameters().size(); ++i)
            EXPECT_NE(model.parameters()[i], clone->parameters()[i])
                << "parameter " << i << " (" << model.parameter_labels()[i] << ")";
    }

    // ── sigma_chi's adjoint matches a common-random-number bump: no branch
    // anywhere in this model (a pure affine-Gaussian update), so there is no
    // discreteness to trip over ──────────────────────────────────────────────

    TEST(SchwartzSmithSimModel, SigmaChiAdjointMatchesCommonRandomNumberBump)
    {
        const Real s0 = 50.0, r = 0.03, chi0 = 0.1, kappa = 1.2, base_sigma_chi = 0.35,
                   mu_xi = 0.02, sigma_xi = 0.18, rho = -0.3, Tm = 1.0;
        const double h = 1e-5;

        LogSpotT<Number> product(Tm);

        auto price_for = [&](double sigma_chi)
        {
            Tape local_tape;
            TapeSwitch guard(local_tape);
            SchwartzSmithSimModel<Number> m{Number(s0), Number(r), Number(chi0), Number(kappa),
                                            Number(sigma_chi), Number(mu_xi), Number(sigma_xi),
                                            Number(rho)};
            m.init(product.timeline(), product.defline());
            Scenario<Number> path;
            allocate_scenario(path, product.defline(), m.n_underlyings());
            std::vector<double> z(m.sim_dim(), 0.3);
            m.generate_path(std::span<const double>(z), path);
            std::vector<Number> payoffs(1);
            product.payoffs(path, payoffs);
            return payoffs[0].value();
        };

        const double bump =
            (price_for(base_sigma_chi + h) - price_for(base_sigma_chi - h)) / (2.0 * h);

        Tape tape;
        TapeSwitch guard(tape);
        Number sigma_chi(base_sigma_chi);
        SchwartzSmithSimModel<Number> model{Number(s0), Number(r), Number(chi0), Number(kappa),
                                            sigma_chi, Number(mu_xi), Number(sigma_xi),
                                            Number(rho)};
        model.init(product.timeline(), product.defline());
        Scenario<Number> path;
        allocate_scenario(path, product.defline(), model.n_underlyings());
        std::vector<double> z(model.sim_dim(), 0.3);
        model.generate_path(std::span<const double>(z), path);
        std::vector<Number> payoffs(1);
        product.payoffs(path, payoffs);
        payoffs[0].propagate_to_start();

        EXPECT_NEAR(sigma_chi.adjoint(), bump, 1e-4);
    }

    TEST(SchwartzSmithSimModel, SimulateAadReportsAllEightParameters)
    {
        Tape tape;
        TapeSwitch guard(tape);

        LogSpotT<Number> product(1.0);
        SchwartzSmithSimModel<Number> model{Number(50.0), Number(0.03), Number(0.1),
                                            Number(1.2), Number(0.35), Number(0.02),
                                            Number(0.18), Number(-0.3)};

        const AADSimulResults res = simulate_aad(product, model, 40000, 4);

        ASSERT_EQ(res.risk_labels.size(), 8u);
        EXPECT_EQ(res.risk_labels[0], "spot");
        EXPECT_EQ(res.risk_labels[1], "rate");
        EXPECT_EQ(res.risk_labels[2], "chi0");
        EXPECT_EQ(res.risk_labels[3], "kappa");
        EXPECT_EQ(res.risk_labels[5], "mu_xi");
        EXPECT_EQ(res.risk_labels[7], "rho");

        EXPECT_TRUE(std::isfinite(res.price));
        for (std::size_t i = 0; i < res.risks.size(); ++i)
            EXPECT_TRUE(std::isfinite(res.risks[i])) << res.risk_labels[i];

        // log(S_T) = chi_T + xi_T is linear in (spot, chi0, mu_xi) and does
        // not depend on rate at all (this payoff reads spots[], not
        // numeraire/discounts) -- so those four risks are deterministic
        // constants, identical on every path, with a checkable closed form
        // and a Monte-Carlo standard error that is legitimately ~0:
        //   d/d(spot)  = d(xi_0)/d(spot) = 1/spot
        //   d/d(rate)  = 0
        //   d/d(chi0)  = e^{-kappa*T} - 1        (chi0's own decay, minus xi_0's -chi0 term)
        //   d/d(mu_xi) = T                       (mean_xi's mu_xi*dt term, summed)
        // kappa, sigma_chi, sigma_xi and rho, by contrast, also shape the
        // *volatility* terms multiplying z1/z2 -- their risk is genuinely
        // path-dependent, checked below only for a positive standard error
        // (an exact reference would need differentiating the whole
        // Cholesky construction by hand, not just reading off a
        // coefficient).
        const Real Tm = 1.0, kappa = 1.2;
        EXPECT_NEAR(res.risks[0], 1.0 / 50.0, 1e-6);
        EXPECT_NEAR(res.risks[1], 0.0, 1e-9);
        EXPECT_NEAR(res.risks[2], std::exp(-kappa * Tm) - 1.0, 1e-6);
        EXPECT_NEAR(res.risks[5], Tm, 1e-6);

        for (std::size_t i : {3u, 4u, 6u, 7u})
            EXPECT_GT(res.risk_std_errors[i], 0.0) << res.risk_labels[i];
    }

} // namespace quantModeling
