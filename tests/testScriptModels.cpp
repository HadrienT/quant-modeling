#include <gtest/gtest.h>

#include "quantModeling/aad/number.hpp"
#include "quantModeling/core/date.hpp"
#include "quantModeling/engines/mc/simulation_engine.hpp"
#include "quantModeling/engines/mc/simulation_engine_aad.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
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

        ValuationContext ctx() { return ValuationContext{Date::from_iso("2024-06-03")}; }

        PricingSettings settings()
        {
            PricingSettings s;
            s.mc_paths = 200000;
            s.mc_seed = 11;
            s.mc_antithetic = true;
            return s;
        }

        double price(const std::string &script, const std::string &model,
                     const Grid &g)
        {
            ScriptedProduct<Real> product(script, ctx());
            auto m = make_script_model<Real>(model, 100.0, 0.03, 0.0, 0.2, g.K,
                                             g.T, g.sigma, 1.0 / 52.0);
            return simulate<Real>(product, *m, settings()).npv();
        }

        const std::string vanilla = "2025-06-03\n    pays max(spot() - 100, 0)\n";
        const std::string down_digital =
            "2025-06-03\n    if spot() < 80 then pays 1000 endIf\n";
    } // namespace

    TEST(ScriptModels, UnknownModelIsRejectedWithTheValidChoices)
    {
        try
        {
            make_script_model<Real>("heston", 100, 0.03, 0, 0.2, {}, {}, {}, 0.02);
            FAIL() << "expected an exception";
        }
        catch (const std::invalid_argument &e)
        {
            const std::string msg = e.what();
            EXPECT_NE(msg.find("heston"), std::string::npos);
            EXPECT_NE(msg.find("local_vol"), std::string::npos);
        }
    }

    TEST(ScriptModels, BlackScholesNeedsAPositiveVol)
    {
        EXPECT_THROW(make_script_model<Real>("black_scholes", 100, 0.03, 0, 0.0,
                                             {}, {}, {}, 0.02),
                     InvalidInput);
    }

    TEST(ScriptModels, LocalVolRejectsAGridWhoseSizesDisagree)
    {
        Grid g = make_grid(0.0);
        g.sigma.pop_back();
        EXPECT_THROW(make_script_model<Real>("local_vol", 100, 0.03, 0, 0.2, g.K,
                                             g.T, g.sigma, 0.02),
                     InvalidInput);
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
        auto m = make_script_model<Number>("local_vol", 100.0, 0.03, 0.0, 0.2,
                                           g.K, g.T, g.sigma, 1.0 / 52.0);
        const AADSimulResults res = simulate_aad(product, *m, 20000, 3);
        aad::Number::tape = nullptr;

        // spot, rate, div + one local vol per (K, T) point
        EXPECT_EQ(res.risks.size(), 3 + g.K.size() * g.T.size());
        EXPECT_EQ(res.risk_labels.front(), "spot");
    }

} // namespace quantModeling
