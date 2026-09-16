#include <gtest/gtest.h>

#include "quantModeling/aad/number.hpp"
#include "quantModeling/engines/mc/simulation_engine.hpp"
#include "quantModeling/engines/mc/simulation_engine_aad.hpp"
#include "quantModeling/instruments/simulatable.hpp"
#include "quantModeling/models/equity/sabr.hpp"
#include "quantModeling/models/equity/sabr_sim_model.hpp"

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

    TEST(SABRSimModel, RejectsNonPositiveForward)
    {
        EXPECT_THROW((SABRSimModel<Real>{0.0, 0.03, 0.25, 0.6, -0.3, 0.5}),
                    InvalidInput);
    }

    TEST(SABRSimModel, RejectsNonPositiveAlpha)
    {
        EXPECT_THROW((SABRSimModel<Real>{100.0, 0.03, 0.0, 0.6, -0.3, 0.5}),
                    InvalidInput);
    }

    TEST(SABRSimModel, RejectsBetaOutOfRange)
    {
        EXPECT_THROW((SABRSimModel<Real>{100.0, 0.03, 0.25, 1.5, -0.3, 0.5}),
                    InvalidInput);
    }

    TEST(SABRSimModel, RejectsRhoOutOfRange)
    {
        EXPECT_THROW((SABRSimModel<Real>{100.0, 0.03, 0.25, 0.6, -1.5, 0.5}),
                    InvalidInput);
    }

    TEST(SABRSimModel, RejectsNegativeNu)
    {
        EXPECT_THROW((SABRSimModel<Real>{100.0, 0.03, 0.25, 0.6, -0.3, -0.5}),
                    InvalidInput);
    }

    // ── cross-check against Hagan's own closed-form implied vol + Black-76,
    // the pre-existing, independently-tested oracle (models/equity/sabr.hpp,
    // already cross-validated against an arbitrage-free PDE solver in
    // testSABRPDE.cpp). Moderate, realistic parameters and an ATM strike --
    // where Hagan's asymptotic formula is most accurate -- keep the formula's
    // own approximation error well inside the tolerance below, alongside
    // this model's Euler discretisation error on F ─────────────────────────

    TEST(SABRSimModel, AtmPriceMatchesHaganBlack76)
    {
        const Real F0 = 100, K = 100, r = 0.03, Tm = 1.0;
        const SABRParams p{/*alpha=*/0.25, /*beta=*/0.6, /*rho=*/-0.3, /*nu=*/0.5};

        EuroCallT<Real> product(K, Tm);
        SABRSimModel<Real> model(F0, r, p.alpha, p.beta, p.rho, p.nu);

        PricingSettings settings;
        settings.mc_paths = 200000;
        settings.mc_seed = 5;
        settings.mc_antithetic = true;
        const SimulationMCResult res = simulate<Real>(product, model, settings);

        const Real vol = sabr_implied_vol(F0, K, Tm, p);
        const Real df = std::exp(-r * Tm);
        const Real expected = black76_call_price(F0, K, Tm, vol, df);

        EXPECT_NEAR(res.npv(), expected, 4.0 * res.std_error() + 0.3);
    }

    // ── the general oracle: F is driftless by construction -- the SDE has
    // no r/q term at all, unlike a spot process, since r here only builds
    // the numeraire convention (see this model's own class doc). So
    // E^Q[F_T/numeraire(T)] = F0*exp(-r*T), not F0 -- exactly the same
    // "dividend leakage" identity already hit for Kou/Bates (issue #37),
    // except here it is r itself playing q's role, since F carries no
    // drift to compensate for the numeraire's own growth. Holds regardless
    // of alpha/beta/rho/nu, the identity the Euler-CEV scheme's absorption
    // at F=0 has to preserve ────────────────────────────────────────────

    TEST(SABRSimModel, DiscountedForwardIsAMartingale)
    {
        const Real F0 = 100, r = 0.03, Tm = 1.0;
        DeflatedSpotT<Real> product(Tm);
        SABRSimModel<Real> model(F0, r, 0.3, 0.4, -0.5, 0.6);

        PricingSettings settings;
        settings.mc_paths = 300000;
        settings.mc_seed = 9;
        settings.mc_antithetic = true;
        const SimulationMCResult res = simulate<Real>(product, model, settings);

        const Real expected = F0 * std::exp(-r * Tm);
        EXPECT_NEAR(res.npv(), expected, 4.0 * res.std_error());
    }

    // ── the AAD trap, closed ─────────────────────────────────────────────

    TEST(SABRSimModel, CloneKeepsItsOwnParameterPointers)
    {
        SABRSimModel<Real> model(100.0, 0.03, 0.25, 0.6, -0.3, 0.5);
        const auto clone = model.clone();

        ASSERT_EQ(model.parameters().size(), clone->parameters().size());
        for (std::size_t i = 0; i < model.parameters().size(); ++i)
            EXPECT_NE(model.parameters()[i], clone->parameters()[i])
                << "parameter " << i << " (" << model.parameter_labels()[i] << ")";
    }

    // ── nu's adjoint matches a common-random-number bump on the same path:
    // nu never affects any discrete decision (there is none in this model --
    // F's absorption at 0 is a continuous clamp, not a branch), so there is
    // no discreteness to trip over here ─────────────────────────────────────

    TEST(SABRSimModel, NuAdjointMatchesCommonRandomNumberBump)
    {
        const Real F0 = 100, r = 0.03, alpha0 = 0.25, beta = 0.6, rho = -0.3,
                   K = 100.0, Tm = 1.0 / 50.0; // one internal step
        const double base_nu = 0.5;
        const double h = 1e-4;

        EuroCallT<Number> product(K, Tm);

        auto price_for = [&](double nu)
        {
            Tape local_tape;
            TapeSwitch guard(local_tape);
            SABRSimModel<Number> m{Number(F0), Number(r), Number(alpha0), beta,
                                   Number(rho), Number(nu)};
            m.init(product.timeline(), product.defline());
            Scenario<Number> path;
            allocate_scenario(path, product.defline(), m.n_underlyings());
            std::vector<double> z(m.sim_dim(), 0.6);
            m.generate_path(std::span<const double>(z), path);
            std::vector<Number> payoffs(1);
            product.payoffs(path, payoffs);
            return payoffs[0].value();
        };

        const double bump =
            (price_for(base_nu + h) - price_for(base_nu - h)) / (2.0 * h);

        Tape tape;
        TapeSwitch guard(tape);
        Number nu(base_nu);
        SABRSimModel<Number> model{Number(F0),    Number(r), Number(alpha0),
                                   beta,          Number(rho), nu};
        model.init(product.timeline(), product.defline());
        Scenario<Number> path;
        allocate_scenario(path, product.defline(), model.n_underlyings());
        std::vector<double> z(model.sim_dim(), 0.6);
        model.generate_path(std::span<const double>(z), path);
        std::vector<Number> payoffs(1);
        product.payoffs(path, payoffs);
        payoffs[0].propagate_to_start();

        EXPECT_NEAR(nu.adjoint(), bump, 1e-4);
    }

    // ── simulate_aad reports all five parameters, correctly labelled ────────

    TEST(SABRSimModel, SimulateAadReportsAllFiveParameters)
    {
        Tape tape;
        TapeSwitch guard(tape);

        EuroCallT<Number> product(100.0, 1.0);
        SABRSimModel<Number> model{Number(100.0), Number(0.03), Number(0.25),
                                   0.6, Number(-0.3), Number(0.5)};

        const AADSimulResults res = simulate_aad(product, model, 50000, 4);

        ASSERT_EQ(res.risk_labels.size(), 5u);
        EXPECT_EQ(res.risk_labels[0], "forward");
        EXPECT_EQ(res.risk_labels[1], "rate");
        EXPECT_EQ(res.risk_labels[2], "alpha");
        EXPECT_EQ(res.risk_labels[3], "rho");
        EXPECT_EQ(res.risk_labels[4], "nu");

        EXPECT_GT(res.risks[0], 0.0); // delta of an ATM call: positive
        for (double se : res.risk_std_errors)
            EXPECT_GT(se, 0.0);
    }

} // namespace quantModeling
