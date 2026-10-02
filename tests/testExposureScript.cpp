#include <gtest/gtest.h>

#include "quantModeling/core/date.hpp"
#include "quantModeling/engines/analytic/hull_white_swaption.hpp"
#include "quantModeling/engines/analytic/swap.hpp"
#include "quantModeling/engines/xva/exposure_engine.hpp"
#include "quantModeling/engines/xva/hull_white_future_value.hpp"
#include "quantModeling/engines/xva/script_future_value.hpp"
#include "quantModeling/risk/exposure_paths.hpp"
#include "quantModeling/scripting/script_error.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

// Lot X4b of blueprint/wp/23-xva.md: the exposure of a trade written as a
// script. The oracle is the same trade as a native instrument: a swap in
// closed form, a Bermudan by the regression of lot X4a.

namespace quantModeling
{
    namespace
    {
        const Date kToday = Date::from_iso("2024-06-03");
        ValuationContext ctx()
        {
            return ValuationContext{kToday};
        }

        /// Year k on the script's calendar: exactly t = k under ACT/365F.
        std::string year(int k)
        {
            return (kToday + 365 * k).to_iso();
        }

        HullWhiteCurveModel model()
        {
            return HullWhiteCurveModel(0.03, 0.01, DiscountCurve(0.04));
        }

        Real par_rate(const HullWhiteCurveModel &m, Time start, Time tenor)
        {
            return value_swap(make_swap(start, tenor, 0.0, 1, 1), MultiCurve{m.discount(), m.projection()})
                .par_rate;
        }

        /**
         * A payer swap of 100, annual on both legs, from `start` to `end`
         * (whole years), as a script: on each coupon date it pays the
         * floating coupon fixed a year earlier against the fixed one, and
         * fixes the next, 1 / df(next date) - 1, in the variable `libor`.
         * A coupon starting today is known today.
         */
        std::string swap_script(const HullWhiteCurveModel &m, int start, int end, Real fixed_rate)
        {
            std::string script;
            if (start > 0)
                script += year(start) + "\n    libor = 1 / df(" + year(start + 1) + ") - 1\n";
            for (int k = start + 1; k <= end; ++k)
            {
                script += year(k) + "\n";
                if (k == 1 && start == 0)
                    script += "    pays 100 * (" +
                              std::to_string(1.0 / m.discount().discount(1.0) - 1.0 - fixed_rate) + ")\n";
                else
                    script += "    pays 100 * (libor - " + std::to_string(fixed_rate) + ")\n";
                if (k < end)
                    script += "    libor = 1 / df(" + year(k + 1) + ") - 1\n";
            }
            return script;
        }

        ExposureSimulationSettings settings(std::size_t paths)
        {
            ExposureSimulationSettings s;
            s.paths = paths;
            s.seed = 7;
            s.keep_cashflows = true;
            return s;
        }

        Real maximum(const std::vector<Real> &v)
        {
            return *std::max_element(v.begin(), v.end());
        }

        Real worst_gap(const std::vector<Real> &a, const std::vector<Real> &b)
        {
            Real worst = 0.0;
            for (std::size_t i = 0; i < a.size(); ++i)
                worst = std::max(worst, std::abs(a[i] - b[i]));
            return worst;
        }
    } // namespace

    TEST(ExposureScript, ASwapWrittenAsAScriptPaysAndIsWorthWhatTheSwapIs)
    {
        const HullWhiteCurveModel m = model();
        // Six decimals in the script: the same rate on both sides.
        const Real K = std::stod(std::to_string(par_rate(m, 0.0, 6.0)));
        HullWhiteExposureEngine engine(m);
        engine.add(make_swap(0.0, 6.0, K, 1, 1, 100.0, true));
        engine.add(make_script_future_value(swap_script(m, 0, 6, K), ctx(), m));
        const ExposurePaths paths = engine.simulate(settings(20000));
        EXPECT_EQ(paths.pilot_paths, 80000u);

        // The same coupons on the same paths: df() read off the state is
        // the model's zero-coupon bond.
        const std::size_t n = paths.dates();
        for (std::size_t c = 0; c < paths.paths * n; c += 13)
            ASSERT_NEAR(paths.trade_cashflows[1][c], paths.trade_cashflows[0][c], 2e-4)
                << "path " << c / n << ", t=" << paths.times[c % n];
        // Today's value: the script's own Monte-Carlo price, against 0 for a
        // par swap.
        EXPECT_NEAR(paths.trade_values_today[1], paths.trade_values_today[0], 0.08);

        // The exposure of the script is the swap's, within the Monte-Carlo
        // error of the regression (as the twin of lot X4a). Between two
        // coupon dates the value depends on the coupon already fixed: the
        // script's variable `libor` is a regressor.
        const ExposureStatistics exact = exposure_statistics(paths, {0}, 0.99);
        const ExposureStatistics script = exposure_statistics(paths, {1}, 0.99);
        EXPECT_LT(worst_gap(script.ee, exact.ee), 0.02 * maximum(exact.ee));
        EXPECT_LT(worst_gap(script.pfe, exact.pfe), 0.04 * maximum(exact.pfe));
        Real gap = 0.0, scale = 0.0;
        for (std::size_t c = 0; c < paths.paths * n; ++c)
        {
            const Real d = paths.trade_values[1][c] - paths.trade_values[0][c];
            gap += d * d;
            scale += paths.trade_values[0][c] * paths.trade_values[0][c];
        }
        EXPECT_LT(std::sqrt(gap / scale), 0.04);
    }

    TEST(ExposureScript, ABermudanWrittenAsAScriptHasTheExposureOfTheNativeOneUntilExercised)
    {
        const HullWhiteCurveModel m = model();
        const Real K = std::stod(std::to_string(par_rate(m, 1.0, 5.0)));
        // The right, on years 1 to 5, to receive the value of what is left of
        // a payer swap ending in year 6 — cash-settled: once exercised the
        // trade is over, which the script records in `done`.
        // Swap value at year e: 1 - df(end) - K Σ df(coupon dates).
        std::string script;
        for (int e = 1; e <= 5; ++e)
        {
            std::string value = "1 - df(" + year(6) + ")";
            for (int k = e + 1; k <= 6; ++k)
                value += " - " + std::to_string(K) + " * df(" + year(k) + ")";
            script += year(e) + "\n    value = 100 * (" + value +
                      ")\n    if exercise(max(value, 0), value) then\n        pays max(value, 0)\n        done = 1\n"
                      "    endIf\n";
        }
        // A last date, after every exercise date, with nothing on it: the
        // grid then runs as long as the native trade's.
        script += year(6) + "\n    done = done\n";

        std::vector<Time> exercise;
        for (int e = 1; e <= 5; ++e)
            exercise.push_back(static_cast<Time>(e));
        const BermudanSwaption native{make_swap(1.0, 5.0, K, 1, 1, 100.0, true), exercise};
        const Real lattice = hull_white_bermudan_swaption(native, m);

        HullWhiteExposureEngine engine(m);
        engine.add(native);
        engine.add(make_script_future_value(script, ctx(), m));
        const ExposurePaths paths = engine.simulate(settings(20000));
        const std::size_t n = paths.dates();

        // Today: the script's Longstaff-Schwartz price, a lower bound close
        // to the lattice.
        EXPECT_LT(paths.trade_values_today[1], lattice * 1.01);
        EXPECT_GT(paths.trade_values_today[1], lattice * 0.97);

        const ExposureStatistics a = exposure_statistics(paths, {0});
        const ExposureStatistics b = exposure_statistics(paths, {1});
        std::size_t first = 0;
        while (paths.times[first] < 1.0 - 1e-9)
            ++first;
        // Until the first exercise date both are the same option.
        for (std::size_t i = 0; i < first; ++i)
        {
            EXPECT_NEAR(b.discounted_ee[i], a.discounted_ee[i], 0.03 * lattice) << paths.times[i];
            EXPECT_GE(b.discounted_ene[i], -0.01 * lattice);
        }
        // Cash-settled: the script pays the swap's value once and is then
        // worth nothing, where the native one becomes the swap.
        std::size_t exercised = 0;
        for (std::size_t p = 0; p < paths.paths; ++p)
        {
            std::size_t paid = n;
            for (std::size_t i = 0; i < n; ++i)
                if (paths.trade_cashflows[1][p * n + i] != 0.0)
                {
                    ASSERT_EQ(paid, n) << "paid twice";
                    ASSERT_GT(paths.trade_cashflows[1][p * n + i], 0.0);
                    paid = i;
                }
            if (paid == n)
                continue;
            ++exercised;
            for (std::size_t i = paid; i < n; ++i)
                ASSERT_EQ(paths.trade_values[1][p * n + i], 0.0);
        }
        EXPECT_GT(exercised, paths.paths / 3);
        EXPECT_LT(exercised, paths.paths);
    }

    TEST(ExposureScript, SameBitsWhateverTheNumberOfThreadsAndDateByDate)
    {
        const HullWhiteCurveModel m = model();
        const std::string script = swap_script(m, 1, 4, 0.04);
        const auto run = [&](ThreadPool *pool)
        {
            HullWhiteExposureEngine engine(m);
            engine.add(make_script_future_value(script, ctx(), m), -2.0);
            ExposureSimulationSettings s = settings(1500);
            s.pilot_paths = 4000;
            return engine.simulate(s, pool);
        };
        const ExposurePaths serial = run(nullptr);
        ThreadPool many;
        many.start(6);
        const ExposurePaths parallel = run(&many);
        EXPECT_EQ(parallel.trade_values, serial.trade_values);
        EXPECT_EQ(parallel.trade_cashflows, serial.trade_cashflows);
        // A forward-starting swap pays nothing before its first coupon.
        const std::size_t n = serial.dates();
        for (std::size_t i = 0; i < n && serial.times[i] < 2.0 - 1e-9; ++i)
            EXPECT_EQ(serial.trade_cashflows[0][i], 0.0);
    }

    TEST(ExposureScript, RejectsWhatTheEngineCannotSimulate)
    {
        const HullWhiteCurveModel m = model();
        // An equity: the engine simulates rates only.
        EXPECT_THROW(make_script_future_value(year(1) + "\n    pays max(spot() - 100, 0)\n", ctx(), m),
                     InvalidInput);
        // Not a script.
        EXPECT_THROW(make_script_future_value("pays 1", ctx(), m), scripting::ScriptError);
        // More variables than a regression takes.
        std::string many = year(1) + "\n";
        for (int v = 0; v < 8; ++v)
            many += "    v" + std::to_string(v) + " = df(" + year(2) + ")\n";
        many += year(2) + "\n    pays v0\n";
        EXPECT_THROW(make_script_future_value(many, ctx(), m), InvalidInput);
        ScriptExposureSettings few;
        few.pricing_paths = 10;
        EXPECT_THROW(make_script_future_value(year(1) + "\n    pays 1\n", ctx(), m, few), InvalidInput);

        // A zero-coupon bond: worth P(0, 2) today, and P(t, 2 | x) at t —
        // the regression of one certain payment recovers the bond.
        HullWhiteExposureEngine engine(m);
        engine.add(make_script_future_value(year(2) + "\n    pays 100\n", ctx(), m));
        const ExposurePaths paths = engine.simulate(settings(5000));
        // (Today's value is the script's Monte-Carlo price: the bank account
        // it is discounted with is simulated.)
        EXPECT_NEAR(paths.trade_values_today[0], 100.0 * m.discount().discount(2.0), 0.01);
        const ExposureStatistics s = exposure_statistics(paths);
        for (std::size_t i = 0; i + 1 < s.times.size(); ++i)
            EXPECT_NEAR(s.discounted_ee[i], 100.0 * m.discount().discount(2.0),
                        4.0 * s.discounted_ee_error[i] + 0.02);
        EXPECT_EQ(s.discounted_ee.back(), 0.0); // paid: nothing left
    }

} // namespace quantModeling
