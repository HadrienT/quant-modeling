#include <gtest/gtest.h>

#include "quantModeling/aad/number.hpp"
#include "quantModeling/engines/mc/simulation_engine.hpp"
#include "quantModeling/engines/mc/simulation_engine_aad.hpp"
#include "quantModeling/instruments/simulatable.hpp"
#include "quantModeling/models/equity/local_vol_sim_model.hpp"
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

        template <class T>
        std::vector<T> flat_grid(std::size_t nK, std::size_t nT, T value)
        {
            return std::vector<T>(nK * nT, value);
        }
    } // namespace

    // ── construction validates its inputs, same rigor as GridLocalVol ──────

    TEST(LocalVolSimModel, RejectsTooSmallAGrid)
    {
        EXPECT_THROW((LocalVolSimModel<Real>{100.0, 0.03, 0.0, {80.0}, {0.5, 1.0}, {0.2, 0.2}}),
                     InvalidInput);
    }

    TEST(LocalVolSimModel, RejectsSigmaSizeMismatch)
    {
        EXPECT_THROW((LocalVolSimModel<Real>{100.0, 0.03, 0.0, {80.0, 120.0}, {0.5, 1.0}, {0.2, 0.2, 0.2}}),
                     InvalidInput);
    }

    // ── a flat grid must reprice Black-Scholes exactly: log-Euler with a
    // constant sigma is the *exact* one-step transition, regardless of how
    // many internal steps add_monitoring_steps() inserts ─────────────────

    TEST(LocalVolSimModel, FlatGridRecoversBlackScholesExactly)
    {
        const Real S0 = 100, K = 105, r = 0.03, q = 0.01, sigma = 0.2, Tm = 1.5;
        const std::vector<Real> K_grid = {50.0, 100.0, 150.0, 200.0};
        const std::vector<Real> T_grid = {0.25, 1.0, 2.0, 3.0};

        EuroCallT<Real> product(K, Tm);
        LocalVolSimModel<Real> model(S0, r, q, K_grid, T_grid,
                                     flat_grid<Real>(K_grid.size(), T_grid.size(), sigma),
                                     1.0 / 12.0);

        PricingSettings settings;
        settings.mc_paths = 200000;
        settings.mc_seed = 7;
        settings.mc_antithetic = true;
        const SimulationMCResult res = simulate<Real>(product, model, settings);

        const double ref = bs_call(S0, K, r, q, sigma, Tm);
        EXPECT_NEAR(res.npv(), ref, 4.0 * res.std_error());
    }

    // ── the AAD trap, closed ─────────────────────────────────────────────

    TEST(LocalVolSimModel, CloneKeepsItsOwnParameterPointers)
    {
        LocalVolSimModel<Real> model(100.0, 0.03, 0.0, {80.0, 120.0},
                                     {0.5, 1.0}, {0.2, 0.22, 0.21, 0.23});
        const auto clone = model.clone();

        ASSERT_EQ(model.parameters().size(), clone->parameters().size());
        for (std::size_t i = 0; i < model.parameters().size(); ++i)
            EXPECT_NE(model.parameters()[i], clone->parameters()[i])
                << "parameter " << i << " (" << model.parameter_labels()[i] << ")";
    }

    // ── one grid corner's AAD sensitivity matches a common-random-number
    // bump of that same corner, on the same path ────────────────────────

    TEST(LocalVolSimModel, GridPointAdjointMatchesCommonRandomNumberBump)
    {
        const Real S0 = 100, r = 0.03, q = 0.0, K = 100.0, Tm = 1.0;
        const std::vector<Real> K_grid = {80.0, 100.0, 120.0};
        const std::vector<Real> T_grid = {0.5, 1.0};
        const double base_vol = 0.2;
        const double h = 1e-4;
        const std::vector<double> z = {0.3, -0.5}; // two monthly-ish Euler steps' worth is plenty for one bump test

        EuroCallT<Number> product(K, Tm);

        auto price_for_center_vol = [&](double v)
        {
            Tape local_tape;
            TapeSwitch guard(local_tape);
            std::vector<Number> grid = {Number(base_vol), Number(v),
                                        Number(base_vol), Number(base_vol),
                                        Number(v), Number(base_vol)};
            LocalVolSimModel<Number> m{Number(S0), Number(r), Number(q),
                                       K_grid, T_grid, grid, 1.0};
            m.init(product.timeline(), product.defline());
            Scenario<Number> path;
            allocate_scenario(path, product.defline(), m.n_underlyings());
            m.generate_path(std::span<const double>(z), path);
            std::vector<Number> payoffs(1);
            product.payoffs(path, payoffs);
            return payoffs[0].value();
        };

        const double bump = (price_for_center_vol(base_vol + h) -
                             price_for_center_vol(base_vol - h)) /
                            (2.0 * h);

        Tape tape;
        TapeSwitch guard(tape);
        Number center(base_vol); // K_grid[1] ("at the money"), both maturities
        std::vector<Number> grid = {Number(base_vol), center,
                                    Number(base_vol), Number(base_vol),
                                    center, Number(base_vol)};
        LocalVolSimModel<Number> model{Number(S0), Number(r), Number(q),
                                       K_grid, T_grid, grid, 1.0};
        model.init(product.timeline(), product.defline());
        Scenario<Number> path;
        allocate_scenario(path, product.defline(), model.n_underlyings());
        model.generate_path(std::span<const double>(z), path);
        std::vector<Number> payoffs(1);
        product.payoffs(path, payoffs);
        payoffs[0].propagate_to_start();

        EXPECT_NEAR(center.adjoint(), bump, 1e-5);
    }

    // ── simulate_aad reports one risk per grid point plus spot/rate/div,
    // correctly labelled, all with a positive standard error ─────────────

    TEST(LocalVolSimModel, SimulateAadReportsARiskPerGridPoint)
    {
        Tape tape;
        TapeSwitch guard(tape);

        const std::vector<Real> K_grid = {80.0, 100.0, 120.0};
        const std::vector<Real> T_grid = {0.5, 1.0};
        EuroCallT<Number> product(100.0, 1.0);
        LocalVolSimModel<Number> model{
            Number(100.0), Number(0.03), Number(0.0), K_grid, T_grid,
            flat_grid<Number>(K_grid.size(), T_grid.size(), Number(0.2)), 1.0 / 12.0};

        const AADSimulResults res = simulate_aad(product, model, 50000, 3);

        ASSERT_EQ(res.risk_labels.size(), 3 + K_grid.size() * T_grid.size());
        EXPECT_EQ(res.risk_labels[0], "spot");
        EXPECT_EQ(res.risk_labels[1], "rate");
        EXPECT_EQ(res.risk_labels[2], "div");
        EXPECT_EQ(res.risk_labels[3], "lvol[0,0]");
        EXPECT_EQ(res.risk_labels.back(), "lvol[2,1]");

        EXPECT_GT(res.risks[0], 0.0); // delta of an ATM call: positive
        for (double se : res.risk_std_errors)
            EXPECT_GT(se, 0.0);
    }

} // namespace quantModeling
