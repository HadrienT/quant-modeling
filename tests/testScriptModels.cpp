#include <gtest/gtest.h>

#include "quantModeling/aad/number.hpp"
#include "quantModeling/core/date.hpp"
#include "quantModeling/engines/mc/simulation_engine.hpp"
#include "quantModeling/engines/mc/simulation_engine_aad.hpp"
#include "quantModeling/engines/analytic/heston_cos.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/market/slv_calibration.hpp"
#include "quantModeling/market/valuation_context.hpp"
#include "quantModeling/scripting/script_model_factory.hpp"
#include "quantModeling/utils/stats.hpp"

#include <cmath>
#include <string>
#include <vector>

namespace quantModeling
{
    namespace
    {
        using scripting::make_script_model;
        using scripting::ScriptModelSpec;

        struct Grid
        {
            std::vector<double> K, T, sigma;
        };

        /// sigma_loc(K, T) = 0.20 + skew * max(0, (100 - K) / 100): flat at
        /// skew = 0, a put-side skew otherwise.
        Grid make_grid(double skew)
        {
            Grid g;
            for (int i = 0; i <= 20; ++i)
                g.K.push_back(50.0 + 5.0 * i);
            g.T = {0.1, 0.5, 1.0, 2.0};
            for (double k : g.K)
                for (std::size_t j = 0; j < g.T.size(); ++j)
                    g.sigma.push_back(0.20 + skew * std::max(0.0, (100.0 - k) / 100.0));
            return g;
        }

        ValuationContext ctx()
        {
            return ValuationContext{Date::from_iso("2024-06-03")};
        }

        PricingSettings settings()
        {
            PricingSettings s;
            s.mc_paths = 200000;
            s.mc_seed = 11;
            s.mc_antithetic = true;
            return s;
        }

        ScriptModelSpec spec(const std::string &model, const Grid &g)
        {
            ScriptModelSpec s;
            s.model = model;
            s.spot = 100.0;
            s.rate = 0.03;
            s.vol = 0.2;
            s.K_grid = g.K;
            s.T_grid = g.T;
            s.sigma_loc_flat = g.sigma;
            s.max_dt = 1.0 / 52.0;
            return s;
        }

        SimulationMCResult run(const std::string &script, const ScriptModelSpec &s)
        {
            ScriptedProduct<Real> product(script, ctx());
            auto m = make_script_model<Real>(s);
            return simulate<Real>(product, *m, settings());
        }

        double price(const std::string &script, const std::string &model,
                     const Grid &g)
        {
            return run(script, spec(model, g)).npv();
        }

        const HestonParams kHeston{/*v0=*/0.04, /*kappa=*/2.0, /*theta=*/0.04,
                                   /*xi=*/0.5, /*rho=*/-0.7};

        const std::string vanilla = "2025-06-03\n    pays max(spot() - 100, 0)\n";
        const std::string down_digital =
            "2025-06-03\n    if spot() < 80 then pays 1000 endIf\n";
    } // namespace

    TEST(ScriptModels, UnknownModelIsRejectedWithTheValidChoices)
    {
        try
        {
            make_script_model<Real>(spec("rough_bergomi", Grid{}));
            FAIL() << "expected an exception";
        }
        catch (const std::invalid_argument &e)
        {
            const std::string msg = e.what();
            EXPECT_NE(msg.find("rough_bergomi"), std::string::npos);
            EXPECT_NE(msg.find("local_vol"), std::string::npos);
            EXPECT_NE(msg.find("slv"), std::string::npos);
        }
    }

    TEST(ScriptModels, BlackScholesNeedsAPositiveVol)
    {
        ScriptModelSpec s = spec("black_scholes", Grid{});
        s.vol = 0.0;
        EXPECT_THROW(make_script_model<Real>(s), InvalidInput);
    }

    TEST(ScriptModels, LocalVolRejectsAGridWhoseSizesDisagree)
    {
        Grid g = make_grid(0.0);
        g.sigma.pop_back();
        EXPECT_THROW(make_script_model<Real>(spec("local_vol", g)), InvalidInput);
    }

    TEST(ScriptModels, AFlatLocalVolSurfaceReproducesBlackScholes)
    {
        // Same script, same vol level, two model classes: a constant local
        // vol is Black-Scholes, so the prices must agree within Monte Carlo
        // noise (the Euler scheme adds a small discretisation bias).
        const Grid flat = make_grid(0.0);
        const double bs = price(vanilla, "black_scholes", flat);
        const double lv = price(vanilla, "local_vol", flat);
        EXPECT_NEAR(lv, bs, 0.15);
    }

    TEST(ScriptModels, ThePutSkewMovesTheDownsideDigitalThatAFlatVolCannotSee)
    {
        // The whole point of choosing the model: a payoff triggered in the
        // left wing is priced very differently once the wing is richer, at
        // the same ATM vol. Flat vol is blind to it.
        const double flat = price(down_digital, "local_vol", make_grid(0.0));
        const double skewed = price(down_digital, "local_vol", make_grid(0.6));
        EXPECT_GT(skewed, flat * 1.10);
    }

    TEST(ScriptModels, LocalVolUnderAADReportsOneVegaPerSurfacePoint)
    {
        using aad::Number;
        aad::Tape tape;
        aad::Number::tape = &tape;

        const Grid g = make_grid(0.3);
        ScriptedProduct<Number> product(vanilla, ctx());
        auto m = make_script_model<Number>(spec("local_vol", g));
        const AADSimulResults res = simulate_aad(product, *m, 20000, 3);
        aad::Number::tape = nullptr;

        // spot, rate, div + one local vol per (K, T) point
        EXPECT_EQ(res.risks.size(), 3 + g.K.size() * g.T.size());
        EXPECT_EQ(res.risk_labels.front(), "spot");
    }

    TEST(ScriptModels, HestonPricesAScriptedVanillaAtItsSemiAnalyticPrice)
    {
        // The script path and the COS formula are independent: the same
        // parameters must give the same call, up to Monte Carlo noise and
        // the Euler scheme's bias on the variance (daily steps here).
        ScriptModelSpec s = spec("heston", Grid{});
        s.heston = kHeston;
        s.max_dt = 1.0 / 252.0;
        const SimulationMCResult res = run(vanilla, s);

        const double T = 1.0; // 2024-06-03 -> 2025-06-03, ACT/365F
        const double cos = heston_cos_price(100.0 * std::exp(0.03 * T), 100.0, T,
                                            std::exp(-0.03 * T), kHeston, true);
        EXPECT_NEAR(res.npv(), cos, 4.0 * res.std_error() + 0.05);
    }

    TEST(ScriptModels, SLVCalibratedToASkewedSurfaceRepricesItsVanillas)
    {
        // Gyongy's promise, through the scripting path: whatever the Heston
        // dynamics underneath, the leverage makes the marginals the local-vol
        // surface's own, so a European script prices the same under both.
        Grid g;
        for (int i = 0; i <= 60; ++i)
            g.K.push_back(40.0 + 2.5 * i);
        g.T = {0.1, 0.25, 0.5, 0.75, 1.0, 1.5};
        for (double k : g.K)
            for (std::size_t j = 0; j < g.T.size(); ++j)
                g.sigma.push_back(0.20 + 0.3 * std::max(0.0, (100.0 - k) / 100.0));

        SLVCalibrationSettings cal;
        cal.n_particles = 100000;
        cal.seed = 5;
        const SLVLeverageGrid lev = calibrate_slv_leverage(100.0, 0.03, 0.0, kHeston,
                                                           g.K, g.T, g.sigma, cal);

        const std::string put = "2025-06-03\n    pays max(90 - spot(), 0)\n";
        ScriptModelSpec slv = spec("slv", g);
        slv.heston = kHeston;
        slv.leverage_flat = lev.leverage;
        slv.max_dt = 1.0 / 50.0;
        const SimulationMCResult under_slv = run(put, slv);
        const SimulationMCResult under_lv = run(put, spec("local_vol", g));

        EXPECT_NEAR(under_slv.npv(), under_lv.npv(),
                    4.0 * (under_slv.std_error() + under_lv.std_error()) + 0.10);
    }

} // namespace quantModeling
