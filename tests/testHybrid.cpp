#include <gtest/gtest.h>

#include "quantModeling/core/date.hpp"
#include "quantModeling/engines/analytic/hull_white_swaption.hpp"
#include "quantModeling/engines/mc/lsm.hpp"
#include "quantModeling/engines/mc/simulation_engine.hpp"
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
        using scripting::ScriptModelSpec;

        const Date kToday = Date::from_iso("2024-06-03");
        ValuationContext ctx()
        {
            return ValuationContext{kToday};
        }

        /// An upward-sloping curve (continuously compounded zero rates 3 % to 4.5 %).
        std::vector<double> curve_times()
        {
            return {0.5, 1, 2, 3, 5, 7, 10, 15, 20};
        }
        std::vector<double> curve_dfs()
        {
            std::vector<double> out;
            for (double t : curve_times())
                out.push_back(std::exp(-(0.03 + 0.0015 * std::min(t, 10.0)) * t));
            return out;
        }

        ScriptModelSpec hybrid(double rho, double hw_sigma = 0.012, double vol = 0.25)
        {
            ScriptModelSpec s;
            s.model = "hull_white";
            s.spot = 100.0;
            s.dividend = 0.01;
            s.vol = vol;
            s.hw_mean_reversion = 0.05;
            s.hw_sigma = hw_sigma;
            s.hw_rho = rho;
            s.curve_times = curve_times();
            s.curve_dfs = curve_dfs();
            return s;
        }

        SimulationMCResult run(const std::string &script, const ScriptModelSpec &spec, int paths = 200000,
                               bool lsm = false)
        {
            ScriptedProduct<Real> product(script, ctx());
            auto model = make_script_model<Real>(spec);
            if (lsm)
                fit_exercise_policy(product, *model, paths, 3);
            PricingSettings s;
            s.mc_paths = paths;
            s.mc_seed = 3;
            return simulate<Real>(product, *model, s);
        }

        double years(const Date &d)
        {
            return (d - kToday) / 365.0;
        }
    } // namespace

    TEST(HullWhiteHybrid, ReproducesTheCurveAndTheSimulatedDiscountFactorsAreConsistent)
    {
        // pays 1 at T is worth P(0, T) whatever the rate vol; so is df(T)
        // read at an earlier date and paid there.
        const Date t1 = kToday + 730, T = kToday + 2555;
        const DiscountCurve curve(curve_times(), curve_dfs(), CurveExtrapolation::FlatForward);
        const auto zcb = run(T.to_iso() + "\n    pays 1\n", hybrid(0.0));
        EXPECT_NEAR(zcb.npv(), curve.discount(years(T)), 4.0 * zcb.std_error() + 1e-12);
        const auto via_df = run(t1.to_iso() + "\n    pays df(" + T.to_iso() + ")\n", hybrid(0.0));
        EXPECT_NEAR(via_df.npv(), curve.discount(years(T)), 4.0 * via_df.std_error() + 1e-12);
    }

    TEST(HullWhiteHybrid, TheEquityForwardHoldsUnderStochasticRates)
    {
        // E[S(T) / B(T)] = S0 e^{-qT}: a forward contract paying S(T) − F is
        // worth zero at F = S0 e^{-qT} / P(0, T), for any correlation.
        const Date T = kToday + 1825;
        const DiscountCurve curve(curve_times(), curve_dfs(), CurveExtrapolation::FlatForward);
        const double F = 100.0 * std::exp(-0.01 * years(T)) / curve.discount(years(T));
        for (const double rho : {-0.5, 0.0, 0.6})
        {
            const auto r = run(T.to_iso() + "\n    pays spot() - " + std::to_string(F) + "\n", hybrid(rho));
            EXPECT_NEAR(r.npv(), 0.0, 4.0 * r.std_error()) << "rho=" << rho;
        }
    }

    TEST(HullWhiteHybrid, PositiveCorrelationRaisesALongDatedCall)
    {
        // With rho > 0, the stock is high when rates (and the bank account)
        // are high: the terminal distribution of S under the forward measure
        // widens, and a call gains (Brigo & Mercurio 2006, §18.3).
        const std::string call = (kToday + 3650).to_iso() + "\n    pays max(spot() - 150, 0)\n";
        const auto low = run(call, hybrid(-0.6));
        const auto high = run(call, hybrid(0.6));
        EXPECT_GT(high.npv() - low.npv(), 4.0 * std::hypot(high.std_error(), low.std_error()));
    }

    TEST(HullWhiteHybrid, ScriptedBermudanSwaptionByLsmcMatchesTheLattice)
    {
        // A 2y-into-5y receiver Bermudan on annual fixed dates, written with
        // df() and exercise(): the swap's value at each date is
        // Σ K δ df(t_i) + df(end) − 1 (single curve, receive fixed).
        const double K = 0.04;
        std::vector<Date> fixed; // 2026 .. 2031, one a year
        for (int y = 0; y <= 5; ++y)
            fixed.push_back(kToday + 730 + 365 * y);
        std::string script;
        for (int e = 0; e < 5; ++e) // exercise on each fixed start
        {
            std::string swap = "-1";
            for (int i = e + 1; i <= 5; ++i)
                swap += " + " + std::to_string(K * (years(fixed[i]) - years(fixed[i - 1]))) + " * df(" +
                        fixed[i].to_iso() + ")";
            swap += " + df(" + fixed[5].to_iso() + ")";
            script += fixed[e].to_iso() + "\n    value = " + swap +
                      "\n    if exercise(max(value, 0), value) then\n        pays value\n    endIf\n";
        }
        ScriptModelSpec spec = hybrid(0.0, 0.01, 0.2);
        const auto lsm = run(script, spec, 100000, true);

        // The same trade on the lattice (engines/analytic/hull_white_swaption.hpp).
        const DiscountCurve curve(curve_times(), curve_dfs(), CurveExtrapolation::FlatForward);
        const HullWhiteCurveModel m(spec.hw_mean_reversion, spec.hw_sigma, curve);
        InterestRateSwap swap;
        swap.fixed_rate = K;
        swap.payer = false;
        std::vector<Time> exercises;
        for (int i = 1; i <= 5; ++i)
        {
            const Time s = years(fixed[i - 1]), e = years(fixed[i]);
            swap.fixed_leg.push_back({s, e, e, e - s});
            swap.floating_leg.push_back({s, e, e, e - s});
            exercises.push_back(s);
        }
        const double lattice = hull_white_bermudan_swaption({swap, exercises}, m);
        // LSMC is a lower bound: within its noise and a small suboptimality.
        EXPECT_LT(lsm.npv(), lattice + 3.0 * lsm.std_error());
        EXPECT_GT(lsm.npv(), lattice * 0.985 - 3.0 * lsm.std_error());
    }

} // namespace quantModeling
