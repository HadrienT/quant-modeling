#include <gtest/gtest.h>

#include "quantModeling/engines/analytic/hull_white_swaption.hpp"
#include "quantModeling/engines/analytic/swap.hpp"
#include "quantModeling/engines/xva/exposure_engine.hpp"
#include "quantModeling/engines/xva/hull_white_future_value.hpp"
#include "quantModeling/engines/xva/regression_future_value.hpp"
#include "quantModeling/market/curve_bootstrap.hpp"
#include "quantModeling/market/multi_curve_bootstrap.hpp"
#include "quantModeling/risk/exposure_paths.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

// Lot X4 of blueprint/wp/23-xva.md: exposure by regression (American
// Monte-Carlo). The closed forms of lot X1 are the oracle: a trade valued by
// regressing its own cash flows must show the exposure its exact value shows.

namespace quantModeling
{
    namespace
    {
        DiscountCurve ois_curve()
        {
            std::vector<ParRateQuote> q;
            for (const auto &[T, r] : std::vector<std::pair<Time, Real>>{
                     {1.0, 0.0400}, {2.0, 0.0385}, {3.0, 0.0378}, {5.0, 0.0375}, {7.0, 0.0380}, {10.0, 0.0390}, {15.0, 0.0400}, {20.0, 0.0402}, {30.0, 0.0395}})
                q.push_back(make_ois_quote(T, r));
            return bootstrap_curve({{0.25, 0.0410}, {0.5, 0.0405}}, q);
        }

        DiscountCurve projection_curve(const DiscountCurve &ois)
        {
            std::vector<ProjectionSwapQuote> swaps;
            for (const auto &[T, r] : std::vector<std::pair<Time, Real>>{
                     {2.0, 0.0412}, {3.0, 0.0405}, {5.0, 0.0402}, {7.0, 0.0406}, {10.0, 0.0415}, {15.0, 0.0424}, {20.0, 0.0426}, {30.0, 0.0420}})
                swaps.push_back(make_projection_swap_quote(T, r));
            return bootstrap_projection_curve(
                ois,
                {{0.0, 0.25, 0.0440, 0.25}, {0.25, 0.5, 0.0432, 0.25}, {0.5, 0.75, 0.0425, 0.25}, {0.75, 1.0, 0.0420, 0.25}},
                swaps);
        }

        HullWhiteCurveModel model()
        {
            const DiscountCurve ois = ois_curve();
            return HullWhiteCurveModel(0.03, 0.01, ois, projection_curve(ois));
        }

        Real par_rate(const HullWhiteCurveModel &m, Time start, Time tenor)
        {
            return value_swap(make_swap(start, tenor, 0.0), MultiCurve{m.discount(), m.projection()})
                .par_rate;
        }

        /// A swap struck at par, annual fixed against quarterly floating: the
        /// coupon in progress makes its value path-dependent.
        InterestRateSwap par_swap(const HullWhiteCurveModel &m, Time start, Time tenor, bool payer)
        {
            return make_swap(start, tenor, par_rate(m, start, tenor), 1, 4, 100.0, payer);
        }

        /// The Bermudan of the tests: the right to enter, on each anniversary
        /// from year 1 to year 9, what is left of a 10-year payer swap.
        BermudanSwaption bermudan(const HullWhiteCurveModel &m)
        {
            std::vector<Time> exercise;
            for (int y = 1; y <= 9; ++y)
                exercise.push_back(static_cast<Time>(y));
            return {par_swap(m, 1.0, 9.0, true), exercise};
        }

        ExposureSimulationSettings settings(std::size_t paths)
        {
            ExposureSimulationSettings s;
            s.paths = paths;
            s.seed = 2026;
            return s;
        }

        Real maximum(const std::vector<Real> &v)
        {
            return *std::max_element(v.begin(), v.end());
        }

        /// Largest |a_i - b_i| over the profile.
        Real worst_gap(const std::vector<Real> &a, const std::vector<Real> &b)
        {
            Real worst = 0.0;
            for (std::size_t i = 0; i < a.size(); ++i)
                worst = std::max(worst, std::abs(a[i] - b[i]));
            return worst;
        }

        /// Root mean square of (regression - exact) over paths and dates,
        /// relative to the root mean square of the exact values.
        Real relative_rms(const std::vector<Real> &regression, const std::vector<Real> &exact)
        {
            Real gap = 0.0, scale = 0.0;
            for (std::size_t c = 0; c < exact.size(); ++c)
            {
                gap += (regression[c] - exact[c]) * (regression[c] - exact[c]);
                scale += exact[c] * exact[c];
            }
            return std::sqrt(gap / scale);
        }

        /// Trade 0 exact, trade 1 its regression twin, on the same paths.
        struct Oracle
        {
            ExposurePaths paths;
            ExposureStatistics exact, regression;
        };

        template <typename Instrument>
        Oracle oracle(const HullWhiteCurveModel &m, const Instrument &instrument,
                      const ExposureSimulationSettings &s, Real quantile = 0.99,
                      const AmcSettings &amc = {})
        {
            HullWhiteExposureEngine engine(m);
            engine.add(instrument);
            engine.add(
                make_regression_future_value(make_hull_white_future_value(instrument, m), amc));
            Oracle out;
            out.paths = engine.simulate(s);
            out.exact = exposure_statistics(out.paths, {0}, quantile);
            out.regression = exposure_statistics(out.paths, {1}, quantile);
            return out;
        }
    } // namespace

    // ── The regression itself ────────────────────────────────────────────────

    TEST(ExposureRegression, APolynomialIsFittedExactlyAndTooFewPointsLowerTheDegree)
    {
        const auto cubic = [](Real x)
        { return 2.0 - x + 0.5 * x * x * x; };
        std::vector<Real> rows, targets;
        for (int k = 0; k < 400; ++k)
        {
            rows.push_back(-1.0 + 0.005 * k);
            targets.push_back(cubic(rows.back()));
        }
        const PolynomialRegression fit = PolynomialRegression::fit(rows, 1, targets, 4);
        EXPECT_EQ(fit.degree(), 4);
        for (const Real x : {-0.9, 0.0, 0.37, 0.95})
            EXPECT_NEAR(fit(&x), cubic(x), 1e-9);

        // Five points cannot carry a quartic: the mean is all they give.
        const std::vector<Real> few_rows{0.0, 1.0, 2.0, 3.0, 4.0}, few{1.0, 2.0, 3.0, 4.0, 5.0};
        const PolynomialRegression mean = PolynomialRegression::fit(few_rows, 1, few, 4);
        EXPECT_EQ(mean.degree(), 0);
        const Real anywhere = 17.0;
        EXPECT_NEAR(mean(&anywhere), 3.0, 1e-12);

        // No point at all: nothing to say, and it says 0.
        const PolynomialRegression none = PolynomialRegression::fit({}, 1, {}, 4);
        EXPECT_TRUE(none.empty());
        EXPECT_EQ(none(&anywhere), 0.0);
        EXPECT_THROW(PolynomialRegression::fit(rows, 2, targets, 2), InvalidInput);
    }

    TEST(ExposureRegression, BucketsFollowAKinkThatOnePolynomialCannot)
    {
        // The value of an option on its expiry date: max(x, 0).
        std::vector<Real> rows, targets;
        for (int k = 0; k < 4000; ++k)
        {
            rows.push_back(-1.0 + 0.0005 * k);
            targets.push_back(std::max(rows.back(), 0.0));
        }
        const BucketedRegression global = BucketedRegression::fit(rows, 1, targets, {4, 1});
        const BucketedRegression local = BucketedRegression::fit(rows, 1, targets, {2, 8});
        EXPECT_EQ(global.buckets(), 1u);
        EXPECT_EQ(local.buckets(), 8u);
        Real worst_global = 0.0, worst_local = 0.0;
        for (std::size_t r = 0; r < rows.size(); ++r)
        {
            worst_global = std::max(worst_global, std::abs(global(&rows[r]) - targets[r]));
            worst_local = std::max(worst_local, std::abs(local(&rows[r]) - targets[r]));
        }
        // The kink falls on a bucket edge here: each side is a straight line.
        EXPECT_LT(worst_local, 1e-9);
        EXPECT_GT(worst_global, 0.02);

        // Too few points for eight buckets of quadratics: fewer buckets.
        const std::vector<Real> few_rows(rows.begin(), rows.begin() + 70);
        const std::vector<Real> few(targets.begin(), targets.begin() + 70);
        EXPECT_EQ(BucketedRegression::fit(few_rows, 1, few, {2, 8}).buckets(), 2u);
        const Real anywhere = 0.3;
        EXPECT_EQ(BucketedRegression::fit({}, 1, {}, {})(&anywhere), 0.0);
        EXPECT_EQ(resolved({}).degree, 2);
        EXPECT_EQ(resolved({}).buckets, 4);
        EXPECT_THROW(resolved({7, 0}), InvalidInput);
        EXPECT_THROW(resolved({0, 65}), InvalidInput);
    }

    // ── The oracle: regression against closed form ───────────────────────────

    TEST(ExposureRegression, ASwapValuedByRegressionShowsTheExposureOfItsClosedForm)
    {
        const HullWhiteCurveModel m = model();
        const Oracle o = oracle(m, par_swap(m, 0.0, 10.0, true), settings(20000));
        EXPECT_EQ(o.paths.pilot_paths, 80000u); // four times the main paths
        const Real peak = maximum(o.exact.ee);
        // The Monte-Carlo error of the exact EE itself is about 1 % of its
        // peak on these paths: the regression stays within it, and within
        // 3 % on the 99 % quantile, where the fit has the least data.
        EXPECT_LT(worst_gap(o.regression.ee, o.exact.ee), 0.015 * peak);
        EXPECT_LT(worst_gap(o.regression.pfe, o.exact.pfe), 0.03 * maximum(o.exact.pfe));
        EXPECT_LT(relative_rms(o.paths.trade_values[1], o.paths.trade_values[0]), 0.025);
    }

    TEST(ExposureRegression, ASwaptionValuedByRegressionFollowsItsExerciseState)
    {
        const HullWhiteCurveModel m = model();
        const Swaption swaption{par_swap(m, 2.0, 8.0, true), 2.0};
        const Oracle o = oracle(m, swaption, settings(20000));
        const Real peak = maximum(o.exact.ee);
        // Three regimes: an option before the expiry, then the swap on the
        // paths that exercised and nothing on the others.
        EXPECT_LT(worst_gap(o.regression.ee, o.exact.ee), 0.015 * peak);
        EXPECT_LT(worst_gap(o.regression.pfe, o.exact.pfe), 0.03 * maximum(o.exact.pfe));
        EXPECT_LT(relative_rms(o.paths.trade_values[1], o.paths.trade_values[0]), 0.025);
    }

    // ── Bermudan swaption: exposure with an exercise state ───────────────────

    TEST(ExposureRegression, ABermudanWithOneExerciseDateIsTheEuropeanSwaption)
    {
        const HullWhiteCurveModel m = model();
        const InterestRateSwap swap = par_swap(m, 2.0, 8.0, true);
        HullWhiteExposureEngine engine(m);
        engine.add(Swaption{swap, 2.0});
        engine.add(BermudanSwaption{swap, {2.0}});
        ExposureSimulationSettings s = settings(20000);
        s.keep_cashflows = true;
        const ExposurePaths paths = engine.simulate(s);
        EXPECT_NEAR(paths.trade_values_today[1], paths.trade_values_today[0],
                    2e-3 * paths.trade_values_today[0]); // lattice against closed form

        // From the expiry on, the rule is the swaption's own (exercise when
        // the swap is worth something): the same swap on the same paths.
        const std::size_t n = paths.dates();
        std::size_t expiry = 0;
        while (paths.times[expiry] < 2.0 - 1e-9)
            ++expiry;
        for (std::size_t p = 0; p < paths.paths; ++p)
            for (std::size_t i = expiry; i < n; ++i)
            {
                ASSERT_DOUBLE_EQ(paths.trade_values[1][p * n + i], paths.trade_values[0][p * n + i]);
                ASSERT_DOUBLE_EQ(paths.trade_cashflows[1][p * n + i],
                                 paths.trade_cashflows[0][p * n + i]);
            }
        // Before it, the continuation value is a regression of the closed form.
        const ExposureStatistics european = exposure_statistics(paths, {0}, 0.99);
        const ExposureStatistics bermudan = exposure_statistics(paths, {1}, 0.99);
        EXPECT_LT(worst_gap(bermudan.ee, european.ee), 0.015 * maximum(european.ee));
        EXPECT_LT(worst_gap(bermudan.pfe, european.pfe), 0.03 * maximum(european.pfe));
    }

    TEST(ExposureRegression, TheBermudansExposureStartsAtItsLatticePriceAndIsAMartingale)
    {
        const HullWhiteCurveModel m = model();
        const BermudanSwaption b = bermudan(m);
        const Real lattice = hull_white_bermudan_swaption(b, m);
        HullWhiteExposureEngine engine(m);
        engine.add(b);
        ExposureSimulationSettings s = settings(40000);
        s.keep_cashflows = true;
        const ExposurePaths paths = engine.simulate(s);
        EXPECT_DOUBLE_EQ(paths.trade_values_today[0], lattice);

        // Discounted value at t_i plus the discounted coupons paid up to t_i:
        // the same expectation at every date — the price of the Bermudan
        // under the fitted exercise rule — whatever the path did in between
        // (option, exercised into a swap, or expired).
        const std::size_t n = paths.dates(), N = paths.paths;
        std::vector<Real> paid(N, 0.0);
        Real first = 0.0, worst = 0.0, error = 0.0;
        for (std::size_t i = 0; i < n; ++i)
        {
            Real sum = 0.0, squares = 0.0;
            for (std::size_t p = 0; p < N; ++p)
            {
                const Real w = paths.discount_weight[p * n + i];
                paid[p] += w * paths.trade_cashflows[0][p * n + i];
                const Real x = w * paths.trade_values[0][p * n + i] + paid[p];
                sum += x;
                squares += x * x;
            }
            const Real mean = sum / static_cast<Real>(N);
            error = std::max(error, std::sqrt((squares / static_cast<Real>(N) - mean * mean) /
                                              static_cast<Real>(N)));
            if (i == 0)
                first = mean;
            worst = std::max(worst, std::abs(mean - first));
        }
        EXPECT_LT(worst, 4.0 * error);
        // An estimated rule is sub-optimal: at most the lattice price, and
        // close to it (the optimum is flat).
        EXPECT_LT(first, lattice + 3.0 * error);
        EXPECT_GT(first, 0.98 * lattice - 3.0 * error);
    }

    TEST(ExposureRegression, ABermudanIsAnAssetUntilExercisedAndASwapAfter)
    {
        const HullWhiteCurveModel m = model();
        const BermudanSwaption b = bermudan(m);
        HullWhiteExposureEngine engine(m);
        engine.add(b);
        engine.add(b, -1.0); // sold: the counterparty holds the right
        // The European swaption on the first exercise date: the Bermudan
        // contains it.
        engine.add(Swaption{b.swap, 1.0});
        ExposureSimulationSettings s = settings(20000);
        s.keep_cashflows = true;
        const ExposurePaths paths = engine.simulate(s);
        const std::size_t n = paths.dates();

        std::size_t first_exercise = 0;
        while (paths.times[first_exercise] < 1.0 - 1e-9)
            ++first_exercise;
        std::size_t exercised = 0, negative_after = 0;
        for (std::size_t p = 0; p < paths.paths; ++p)
        {
            bool paid = false, below_zero = false;
            for (std::size_t i = 0; i < n; ++i)
            {
                const Real v = paths.trade_values[0][p * n + i];
                // Sold = minus bought, path by path.
                ASSERT_EQ(paths.trade_values[1][p * n + i], -v);
                if (i < first_exercise)
                {
                    ASSERT_GE(v, 0.0); // an option that cost its premium
                    ASSERT_EQ(paths.trade_cashflows[0][p * n + i], 0.0);
                }
                paid = paid || paths.trade_cashflows[0][p * n + i] != 0.0;
                below_zero = below_zero || v < 0.0;
            }
            exercised += paid;
            // Only a swap can be worth less than nothing.
            if (below_zero)
            {
                ASSERT_TRUE(paid);
                ++negative_after;
            }
        }
        // At the money, with nine chances: exercised on most paths but not all.
        EXPECT_GT(exercised, paths.paths / 2);
        EXPECT_LT(exercised, paths.paths);
        EXPECT_GT(negative_after, paths.paths / 20);

        const ExposureStatistics bought = exposure_statistics(paths, {0});
        const ExposureStatistics sold = exposure_statistics(paths, {1});
        const ExposureStatistics european = exposure_statistics(paths, {2});
        for (std::size_t i = 0; i < first_exercise; ++i)
        {
            // Nothing is owed to the buyer's counterparty, nothing to the seller.
            EXPECT_EQ(bought.discounted_ene[i], 0.0);
            EXPECT_EQ(sold.discounted_ee[i], 0.0);
            // More rights are worth more: EE* of the Bermudan >= the European's.
            EXPECT_GT(bought.discounted_ee[i],
                      european.discounted_ee[i] - 3.0 * bought.discounted_ee_error[i]);
        }
        EXPECT_GT(paths.trade_values_today[0], paths.trade_values_today[2]);
    }

    // ── The pilot ────────────────────────────────────────────────────────────

    TEST(ExposureRegression, NoPilotWithoutATradeValuedByRegression)
    {
        const HullWhiteCurveModel m = model();
        HullWhiteExposureEngine engine(m);
        engine.add(par_swap(m, 0.0, 5.0, true));
        EXPECT_EQ(engine.simulate(settings(500)).pilot_paths, 0u);
        engine.add(bermudan(m));
        ExposureSimulationSettings s = settings(500);
        EXPECT_GE(engine.simulate(s).pilot_paths, 20000u);
        s.pilot_paths = 3000;
        EXPECT_EQ(engine.simulate(s).pilot_paths, 3000u);
        EXPECT_EQ(engine.pilot_dispersion(), 0.0);
        // The pilot counts against the memory limit, like the cube.
        s.pilot_paths = 0;
        s.memory_limit_bytes = std::size_t{2} << 20;
        EXPECT_THROW(engine.simulate(s), InvalidInput);
    }

    TEST(ExposureRegression, SameBitsWhateverTheNumberOfThreads)
    {
        const HullWhiteCurveModel m = model();
        const auto run = [&](ThreadPool *pool)
        {
            HullWhiteExposureEngine engine(m);
            engine.add(bermudan(m));
            engine.add(make_regression_future_value(
                make_hull_white_future_value(par_swap(m, 0.0, 7.0, false), m)));
            ExposureSimulationSettings s = settings(3000); // not a multiple of the chunk size
            s.pilot_paths = 5000;
            s.keep_cashflows = true;
            return engine.simulate(s, pool);
        };
        const ExposurePaths serial = run(nullptr);
        ThreadPool many;
        many.start(8);
        const ExposurePaths parallel = run(&many);
        EXPECT_EQ(parallel.trade_values, serial.trade_values);
        EXPECT_EQ(parallel.trade_cashflows, serial.trade_cashflows);
    }

    TEST(ExposureRegression, UnderTheHistoricalMeasureThePilotCoversWhereTheScenariosGo)
    {
        // A flat 4 % curve: the historical measure needs a short rate to
        // start from, which a curve held flat before its first pillar lacks.
        const HullWhiteCurveModel m(0.03, 0.01, DiscountCurve(0.04));
        // Rates pulled to zero within a year or two, from forwards at 4 %:
        // scenarios the pricing measure hardly ever visits that early.
        ExposureSimulationSettings s = settings(20000);
        s.historical = HistoricalRateDynamics{1.0, 0.0, 0.006};
        const InterestRateSwap swap = par_swap(m, 0.0, 10.0, true);
        const auto run = [&](std::optional<Real> dispersion)
        {
            HullWhiteExposureEngine engine(m);
            engine.add(swap);
            engine.add(make_regression_future_value(make_hull_white_future_value(swap, m)));
            ExposureSimulationSettings with = s;
            with.pilot_dispersion = dispersion;
            const ExposurePaths paths = engine.simulate(with);
            EXPECT_EQ(paths.measure, ExposureMeasure::Historical);
            const ExposureStatistics exact = exposure_statistics(paths, {0});
            const ExposureStatistics regression = exposure_statistics(paths, {1});
            // A payer swap when rates collapse: deep out of the money.
            const Real scale = -*std::min_element(exact.ene.begin(), exact.ene.end());
            return std::pair{engine.pilot_dispersion(),
                             relative_rms(paths.trade_values[1], paths.trade_values[0])};
        };
        const auto [automatic, covered] = run(std::nullopt);
        const auto [none, uncovered] = run(0.0);
        // The pilot had to start from a dispersed state...
        EXPECT_GT(automatic, 0.005);
        EXPECT_EQ(none, 0.0);
        // ...with which the regression prices these scenarios as the closed
        // form does, path by path. Fitted only where the pricing measure
        // goes, it extrapolates: a swap's value is smooth enough for that to
        // hold up, at close to twice the error.
        EXPECT_LT(covered, 0.01);
        EXPECT_GT(uncovered, 1.5 * covered);

        // A Bermudan runs under the historical measure too.
        HullWhiteExposureEngine engine(m);
        engine.add(bermudan(m));
        ExposureSimulationSettings small = s;
        small.paths = 2000;
        EXPECT_EQ(engine.simulate(small).measure, ExposureMeasure::Historical);
        EXPECT_GT(engine.pilot_dispersion(), 0.005);
        small.pilot_dispersion = -1.0;
        EXPECT_THROW(engine.simulate(small), InvalidInput);
    }

} // namespace quantModeling
