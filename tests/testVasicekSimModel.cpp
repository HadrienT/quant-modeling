#include <gtest/gtest.h>

#include "quantModeling/aad/number.hpp"
#include "quantModeling/engines/mc/simulation_engine.hpp"
#include "quantModeling/engines/mc/simulation_engine_aad.hpp"
#include "quantModeling/instruments/simulatable.hpp"
#include "quantModeling/models/rates/vasicek.hpp"
#include "quantModeling/models/rates/vasicek_sim_model.hpp"

#include <cmath>
#include <span>
#include <vector>

// issue #37, "rate models in the timeline architecture": VasicekSimModel<T>
// is the first short-rate model wired for AAD this way. Hull-White with a
// constant theta is the same process (b = theta/a -- see
// models/rates/hull_white.hpp), so this one model covers both.

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

        /// Pays 1 unit of numeraire at maturity, deflated: E^Q[1/N(T)] is
        /// exactly the zero-coupon bond price P(0,T) by definition of the
        /// numeraire -- the fundamental identity this whole model's exact
        /// joint (r, integral of r) simulation has to reproduce.
        template <class T>
        struct DiscountedUnitPayoffT final : ISimulatableProduct<T>
        {
            TimeLine tl_;
            std::vector<SampleDef> dl_;
            std::vector<std::string> labels_{"price"};

            explicit DiscountedUnitPayoffT(Real maturity)
                : tl_{maturity}, dl_(1) {}
            const TimeLine &timeline() const override { return tl_; }
            const std::vector<SampleDef> &defline() const override { return dl_; }
            const std::vector<std::string> &payoff_labels() const override
            {
                return labels_;
            }
            std::size_t n_underlyings() const override { return 1; }
            void payoffs(const Scenario<T> &p, std::vector<T> &out) const override
            {
                out.assign(1, T(1.0) / p[0].numeraire);
            }
        };
    } // namespace

    TEST(VasicekSimModel, RejectsNonPositiveMeanReversion)
    {
        EXPECT_THROW((VasicekSimModel<Real>{0.03, 0.0, 0.04, 0.01}), InvalidInput);
    }

    TEST(VasicekSimModel, RejectsNonPositiveVol)
    {
        EXPECT_THROW((VasicekSimModel<Real>{0.03, 0.5, 0.04, 0.0}), InvalidInput);
    }

    // ── the cheapest, strongest check: no Monte-Carlo noise at all. The
    // T-typed A(tau)*exp(-B(tau)*r0) ported into VasicekSimModel must
    // reproduce VasicekModel's own closed-form ZCB price to machine
    // precision -- proves the formula was ported correctly before any
    // simulation even runs ─────────────────────────────────────────────────

    TEST(VasicekSimModel, ZcbPriceMatchesTheReferenceClosedFormExactly)
    {
        const Real r0 = 0.03, a = 0.5, b = 0.04, sigma = 0.01;
        VasicekModel reference(a, b, sigma, r0);
        VasicekSimModel<Real> model(r0, a, b, sigma);

        for (Real T : {0.25, 1.0, 5.0, 10.0})
            EXPECT_NEAR(model.zcb_price(T), reference.zcb_price(T), 1e-12);
    }

    // ── the identity the joint (r, integral of r) simulation must satisfy:
    // a unit payoff discounted by the *realized* numeraire averages to
    // exactly the zero-coupon bond price, for every maturity -- this is
    // what actually exercises the Cholesky-style joint draw, not just the
    // discount formula above ────────────────────────────────────────────────

    TEST(VasicekSimModel, DiscountedUnitPayoffMatchesZcbPrice)
    {
        const Real r0 = 0.03, a = 0.5, b = 0.04, sigma = 0.015;

        for (Real Tm : {0.5, 2.0, 7.0})
        {
            DiscountedUnitPayoffT<Real> product(Tm);
            VasicekSimModel<Real> model(r0, a, b, sigma);

            PricingSettings settings;
            settings.mc_paths = 100000;
            settings.mc_seed = 3;
            settings.mc_antithetic = true;
            const SimulationMCResult res = simulate<Real>(product, model, settings);

            const Real expected = model.zcb_price(Tm);
            EXPECT_NEAR(res.npv(), expected, 4.0 * res.std_error())
                << "maturity " << Tm;
        }
    }

    TEST(VasicekSimModel, CloneKeepsItsOwnParameterPointers)
    {
        VasicekSimModel<Real> model(0.03, 0.5, 0.04, 0.01);
        const auto clone = model.clone();

        ASSERT_EQ(model.parameters().size(), clone->parameters().size());
        for (std::size_t i = 0; i < model.parameters().size(); ++i)
            EXPECT_NE(model.parameters()[i], clone->parameters()[i])
                << "parameter " << i << " (" << model.parameter_labels()[i] << ")";
    }

    // ── sigma's adjoint matches a common-random-number bump on the same
    // path: no branch anywhere in this model (no clamp, no absorption), so
    // there is no discreteness to trip over ─────────────────────────────────

    TEST(VasicekSimModel, VolAdjointMatchesCommonRandomNumberBump)
    {
        const Real r0 = 0.03, a = 0.5, b = 0.04, base_sigma = 0.01, Tm = 2.0;
        const double h = 1e-5;

        DiscountedUnitPayoffT<Number> product(Tm);

        auto price_for = [&](double sigma)
        {
            Tape local_tape;
            TapeSwitch guard(local_tape);
            VasicekSimModel<Number> m{Number(r0), Number(a), Number(b), Number(sigma)};
            m.init(product.timeline(), product.defline());
            Scenario<Number> path;
            allocate_scenario(path, product.defline(), m.n_underlyings());
            std::vector<double> z(m.sim_dim(), 0.4);
            m.generate_path(std::span<const double>(z), path);
            std::vector<Number> payoffs(1);
            product.payoffs(path, payoffs);
            return payoffs[0].value();
        };

        const double bump = (price_for(base_sigma + h) - price_for(base_sigma - h)) / (2.0 * h);

        Tape tape;
        TapeSwitch guard(tape);
        Number sigma(base_sigma);
        VasicekSimModel<Number> model{Number(r0), Number(a), Number(b), sigma};
        model.init(product.timeline(), product.defline());
        Scenario<Number> path;
        allocate_scenario(path, product.defline(), model.n_underlyings());
        std::vector<double> z(model.sim_dim(), 0.4);
        model.generate_path(std::span<const double>(z), path);
        std::vector<Number> payoffs(1);
        product.payoffs(path, payoffs);
        payoffs[0].propagate_to_start();

        EXPECT_NEAR(sigma.adjoint(), bump, 1e-4);
    }

    // ── simulate_aad reports all four parameters; r0's risk matches the
    // closed form dP/dr0 = -B(T)*P(0,T), differentiated by hand ────────────

    TEST(VasicekSimModel, SimulateAadReportsAllFourParametersAndR0RiskMatchesClosedForm)
    {
        const Real r0 = 0.03, a = 0.5, b = 0.04, sigma = 0.01, Tm = 3.0;

        Tape tape;
        TapeSwitch guard(tape);

        DiscountedUnitPayoffT<Number> product(Tm);
        VasicekSimModel<Number> model{Number(r0), Number(a), Number(b), Number(sigma)};

        const AADSimulResults res = simulate_aad(product, model, 100000, 6);

        ASSERT_EQ(res.risk_labels.size(), 4u);
        EXPECT_EQ(res.risk_labels[0], "r0");
        EXPECT_EQ(res.risk_labels[1], "mean_reversion");
        EXPECT_EQ(res.risk_labels[2], "long_term_rate");
        EXPECT_EQ(res.risk_labels[3], "vol");

        const VasicekModel reference(a, b, sigma, r0);
        const Real B_T = reference.B(Tm);
        const Real expected_dP_dr0 = -B_T * reference.zcb_price(Tm);

        EXPECT_NEAR(res.risks[0], expected_dP_dr0, 4.0 * res.risk_std_errors[0]);
        for (double se : res.risk_std_errors)
            EXPECT_GT(se, 0.0);
    }

} // namespace quantModeling
