#include <gtest/gtest.h>

#include "quantModeling/engines/analytic/swap.hpp"
#include "quantModeling/engines/xva/exposure_engine.hpp"
#include "quantModeling/market/csa.hpp"
#include "quantModeling/risk/collateral.hpp"
#include "quantModeling/risk/exposure_paths.hpp"
#include "quantModeling/risk/initial_margin.hpp"
#include "quantModeling/risk/xva.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

// Lot X5 of blueprint/wp/23-xva.md: dynamic initial margin by regression,
// MVA and ColVA. The margin is backtested on paths it was not fitted on.

namespace quantModeling
{
    namespace
    {
        constexpr Time kMpor = 10.0 / 250.0;

        HullWhiteCurveModel model()
        {
            return HullWhiteCurveModel(0.03, 0.01, DiscountCurve(0.04));
        }

        InterestRateSwap par_swap(const HullWhiteCurveModel &m, Time tenor, bool payer,
                                  Real notional = 100.0)
        {
            const Real par =
                value_swap(make_swap(0.0, tenor, 0.0, 1, 1), MultiCurve{m.discount(), m.projection()})
                    .par_rate;
            return make_swap(0.0, tenor, par, 1, 1, notional, payer);
        }

        /// A 10-year payer swap and a 5-year receiver, on a grid with the
        /// lagged dates and the cash flows kept.
        ExposurePaths simulate(const HullWhiteCurveModel &m, std::uint64_t seed, std::size_t paths = 20000,
                               Real quantity = 1.0)
        {
            HullWhiteExposureEngine engine(m);
            engine.add(par_swap(m, 10.0, true), quantity);
            engine.add(par_swap(m, 5.0, false, 60.0), quantity);
            ExposureSimulationSettings s;
            s.paths = paths;
            s.seed = seed;
            s.keep_cashflows = true;
            s.grid.margin_period_of_risk = kMpor;
            return engine.simulate(s);
        }
    } // namespace

    // ── The margin against what it is meant to cover ─────────────────────────

    TEST(InitialMargin, BacktestOnFreshPathsTheMoveExceedsTheMarginOncePerHundred)
    {
        const HullWhiteCurveModel m = model();
        const ExposurePaths fitted = simulate(m, 1), fresh = simulate(m, 2);
        const DynamicInitialMargin dim = DynamicInitialMargin::fit(fitted, kMpor);
        const InitialMargin im = dim.margin(fresh);
        const MarginBacktest backtest = backtest_initial_margin(fresh, im, kMpor);

        Real up = 0.0, down = 0.0;
        std::size_t dates = 0;
        for (std::size_t r = 0; r < backtest.times.size(); ++r)
        {
            if (!backtest.full_period[r])
            {
                // A shorter period under a margin sized for ten days: covered
                // more often than a full one.
                EXPECT_LT(backtest.uncovered_bank[r], 0.01);
                continue;
            }
            // After the last fixing of the 10-year swap its value hardly
            // moves: nothing left for a margin to cover (see the header).
            if (backtest.times[r] > 9.0)
                continue;
            // 99 %: one path in a hundred on each side, date by date...
            EXPECT_GT(backtest.uncovered_bank[r], 0.005) << backtest.times[r];
            EXPECT_LT(backtest.uncovered_bank[r], 0.016) << backtest.times[r];
            EXPECT_GT(backtest.uncovered_counterparty[r], 0.005) << backtest.times[r];
            EXPECT_LT(backtest.uncovered_counterparty[r], 0.016) << backtest.times[r];
            up += backtest.uncovered_bank[r];
            down += backtest.uncovered_counterparty[r];
            ++dates;
        }
        ASSERT_GT(dates, 30u);
        // ...and on average over the dates and the two sides.
        const Real pooled = (up + down) / static_cast<Real>(2 * dates);
        EXPECT_NEAR(pooled, 0.01, 0.0015);
    }

    TEST(InitialMargin, ALowerConfidenceIsExceededMoreOften)
    {
        const HullWhiteCurveModel m = model();
        const ExposurePaths fitted = simulate(m, 1, 10000), fresh = simulate(m, 2, 10000);
        DimSettings settings;
        settings.confidence = 0.95;
        const InitialMargin loose = DynamicInitialMargin::fit(fitted, kMpor, {}, settings).margin(fresh);
        const InitialMargin tight = DynamicInitialMargin::fit(fitted, kMpor).margin(fresh);
        EXPECT_NEAR(loose.today / tight.today, 1.6449 / 2.3263, 1e-3); // the two normal quantiles
        const MarginBacktest backtest = backtest_initial_margin(fresh, loose, kMpor);
        Real sum = 0.0;
        std::size_t dates = 0;
        for (std::size_t r = 0; r < backtest.times.size(); ++r)
            if (backtest.full_period[r] && backtest.times[r] <= 9.0)
            {
                sum += backtest.uncovered_bank[r] + backtest.uncovered_counterparty[r];
                dates += 2;
            }
        EXPECT_NEAR(sum / static_cast<Real>(dates), 0.05, 0.005);
    }

    // ── What the margin is a function of ─────────────────────────────────────

    TEST(InitialMargin, TheProfileRunsOffWithTheSwapsAndScalesWithThePosition)
    {
        const HullWhiteCurveModel m = model();
        const ExposurePaths paths = simulate(m, 1, 10000);
        const InitialMargin im = DynamicInitialMargin::fit(paths, kMpor).margin(paths);
        ASSERT_EQ(im.times.size(), collateral_reporting_dates(paths, kMpor).size());
        EXPECT_GT(im.today, 0.0);
        // Before the first lagged date, the margin is today's on every path.
        EXPECT_DOUBLE_EQ(im.margin[0], im.today);
        EXPECT_DOUBLE_EQ(im.expected[0] * paths.discount[0], im.discounted_expected[0]);
        // A swap's sensitivity runs off with its remaining coupons.
        const auto at = [&](Time t)
        {
            std::size_t r = 0;
            while (im.times[r] < t - 1e-9)
                ++r;
            return im.expected[r];
        };
        EXPECT_GT(at(6.5), at(8.5));
        EXPECT_GT(at(8.5), at(9.5));
        EXPECT_LT(im.expected.back(), 0.02 * im.today);
        for (const Real x : im.margin)
            ASSERT_GE(x, 0.0);

        // Twice the position, twice the margin, on every path.
        const ExposurePaths doubled = simulate(m, 1, 10000, 2.0);
        const InitialMargin twice = DynamicInitialMargin::fit(doubled, kMpor).margin(doubled);
        EXPECT_NEAR(twice.today, 2.0 * im.today, 1e-9 * im.today);
        for (std::size_t c = 0; c < im.margin.size(); c += 997)
            EXPECT_NEAR(twice.margin[c], 2.0 * im.margin[c], 1e-6 * im.today);

        // Netting: the receiver offsets the payer, so the set needs less
        // margin than the payer alone.
        EXPECT_LT(im.today, DynamicInitialMargin::fit(paths, kMpor, {0}).today());
    }

    TEST(InitialMargin, AnchoredOnTodaysAmountTheWholeProfileScales)
    {
        const HullWhiteCurveModel m = model();
        const ExposurePaths paths = simulate(m, 1, 5000);
        const DynamicInitialMargin plain = DynamicInitialMargin::fit(paths, kMpor);
        DimSettings settings;
        settings.im_today = 3.0; // a SIMM amount computed elsewhere
        const DynamicInitialMargin anchored = DynamicInitialMargin::fit(paths, kMpor, {}, settings);
        EXPECT_DOUBLE_EQ(anchored.today(), 3.0);
        EXPECT_DOUBLE_EQ(anchored.scaling(), 3.0 / plain.today());
        const InitialMargin a = plain.margin(paths), b = anchored.margin(paths);
        for (std::size_t r = 0; r < a.times.size(); ++r)
            EXPECT_NEAR(b.expected[r], anchored.scaling() * a.expected[r], 1e-12 * (1.0 + a.expected[r]));
        EXPECT_DOUBLE_EQ(plain.scaled(2.0).today(), 2.0 * plain.today());
    }

    TEST(InitialMargin, RejectsWhatItCannotUse)
    {
        const HullWhiteCurveModel m = model();
        const ExposurePaths paths = simulate(m, 1, 2000);
        DimSettings settings;
        settings.confidence = 0.4;
        EXPECT_THROW(DynamicInitialMargin::fit(paths, kMpor, {}, settings), InvalidInput);
        settings = {};
        settings.im_today = -1.0;
        EXPECT_THROW(DynamicInitialMargin::fit(paths, kMpor, {}, settings), InvalidInput);
        EXPECT_THROW(DynamicInitialMargin::fit(paths, kMpor, {5}), InvalidInput);
        EXPECT_THROW(DynamicInitialMargin::fit(paths, 0.0), InvalidInput);
        // A grid built without the lagged dates of this MPoR.
        EXPECT_THROW(DynamicInitialMargin::fit(paths, 0.0123), InvalidInput);
        const DynamicInitialMargin dim = DynamicInitialMargin::fit(paths, kMpor);
        EXPECT_THROW(dim.scaled(-1.0), InvalidInput);
        // Paths on another grid.
        HullWhiteExposureEngine engine(m);
        engine.add(par_swap(m, 3.0, true));
        ExposureSimulationSettings s;
        s.paths = 100;
        EXPECT_THROW(dim.margin(engine.simulate(s)), InvalidInput);
        InitialMargin wrong = dim.margin(paths);
        wrong.paths = 1;
        EXPECT_THROW(backtest_initial_margin(paths, wrong, kMpor), InvalidInput);
    }

    // ── Exposure under initial margin, MVA, ColVA ────────────────────────────

    TEST(InitialMargin, UnderInitialMarginTheExposureIsWhatTheMarginLeavesUncovered)
    {
        const HullWhiteCurveModel m = model();
        const ExposurePaths paths = simulate(m, 1);
        Csa csa; // zero thresholds, no minimum transfer amount: a perfect CSA
        csa.margin_period_of_risk = kMpor;
        // Flows of the period stay in the close-out: the move the margin was
        // sized on.
        CollateralSettings variation_only;
        variation_only.cashflows = MarginPeriodCashflows::Withheld;
        const InitialMargin im = DynamicInitialMargin::fit(paths, kMpor).margin(paths);
        CollateralSettings with_margin = variation_only;
        with_margin.initial_margin_received_paths = im.margin;
        with_margin.initial_margin_posted_paths = im.margin;

        const ExposurePaths vm = collateralise(paths, csa, {}, variation_only);
        const ExposurePaths vm_im = collateralise(paths, csa, {}, with_margin);
        const ExposureStatistics a = exposure_statistics(vm), b = exposure_statistics(vm_im);
        // Variation margin leaves the ten-day move; initial margin leaves the
        // tail of it beyond 99 %: a few percent of the exposure.
        const Real peak = *std::max_element(a.ee.begin(), a.ee.end());
        EXPECT_LT(*std::max_element(b.ee.begin(), b.ee.end()), 0.03 * peak);
        // On each date, some exposure is left on about one path in a hundred.
        const std::size_t dates = vm_im.dates();
        std::size_t checked = 0;
        for (std::size_t r = 0; r < dates; ++r)
        {
            if (vm_im.times[r] < kMpor || vm_im.times[r] > 9.0)
                continue;
            std::size_t exposed = 0;
            for (std::size_t p = 0; p < vm_im.paths; ++p)
                exposed += vm_im.trade_values[0][p * dates + r] > 0.0;
            const Real share = static_cast<Real>(exposed) / static_cast<Real>(vm_im.paths);
            EXPECT_GT(share, 0.004) << vm_im.times[r];
            EXPECT_LT(share, 0.018) << vm_im.times[r];
            ++checked;
        }
        EXPECT_GT(checked, 30u);
        // A path-dependent margin of the wrong shape is refused.
        with_margin.initial_margin_received_paths.pop_back();
        EXPECT_THROW(collateralise(paths, csa, {}, with_margin), InvalidInput);
    }

    TEST(InitialMargin, MvaIsTheFundingOfTheMarginPosted)
    {
        const HullWhiteCurveModel m = model();
        const ExposurePaths paths = simulate(m, 1, 5000);
        const InitialMargin im = DynamicInitialMargin::fit(paths, kMpor).margin(paths);
        const CreditCurve safe(0.0), risky(0.02);
        const Real spread = 0.005;
        // Without default: minus the spread times the time integral of the
        // discounted expected margin.
        Real integral = 0.0;
        Time previous = 0.0;
        for (std::size_t r = 0; r < im.times.size(); ++r)
        {
            integral += im.discounted_expected[r] * (im.times[r] - previous);
            previous = im.times[r];
        }
        const Real cost = mva(im.times, im.discounted_expected, safe, safe, spread);
        EXPECT_NEAR(cost, -spread * integral, 1e-12 * integral);
        EXPECT_LT(cost, 0.0);
        EXPECT_EQ(mva(im.times, im.discounted_expected, safe, safe, 0.0), 0.0);
        // The cost stops when either party defaults.
        EXPECT_GT(mva(im.times, im.discounted_expected, risky, safe, spread), cost);
        // Order of magnitude: about today's margin funded over the weighted
        // life of the profile.
        EXPECT_GT(-cost, spread * im.today * 3.0);
        EXPECT_LT(-cost, spread * im.today * 10.0);
    }

    TEST(InitialMargin, ColvaIsTheCostOfTheRatePaidOnTheCollateralHeld)
    {
        const HullWhiteCurveModel m = model();
        HullWhiteExposureEngine engine(m);
        // A payer swap struck 1 % below the market: an asset, so the bank
        // holds collateral.
        InterestRateSwap swap = par_swap(m, 10.0, true);
        swap.fixed_rate -= 0.01;
        engine.add(swap);
        ExposureSimulationSettings s;
        s.paths = 5000;
        s.grid.margin_period_of_risk = kMpor;
        const ExposurePaths paths = engine.simulate(s);
        Csa csa;
        csa.margin_period_of_risk = kMpor;
        const std::vector<std::size_t> reporting = collateral_reporting_dates(paths, kMpor);
        std::vector<Time> times;
        for (const std::size_t i : reporting)
            times.push_back(paths.times[i]);

        const std::vector<Real> held = discounted_expected_collateral(paths, csa);
        ASSERT_EQ(held.size(), times.size());
        // Under a perfect CSA the collateral is the value ten days earlier:
        // its discounted expectation is that of the value at the lagged date,
        // up to ten days of discounting.
        const ExposureStatistics open = exposure_statistics(paths);
        const std::vector<MarginPeriod> periods = margin_periods(paths.times, kMpor);
        for (std::size_t r = 0; r < times.size(); r += 5)
            if (periods[r].lagged != MarginPeriod::kToday)
                EXPECT_NEAR(held[r], open.discounted_efv[periods[r].lagged],
                            0.01 * paths.trade_values_today[0] +
                                4.0 * open.discounted_efv_error[periods[r].lagged]);
        EXPECT_GT(held.front(), 0.0);

        const CreditCurve safe(0.0);
        // The CSA pays 25 bp above the discount rate on what the bank holds:
        // a cost. 25 bp below: a gain of the same size.
        const Real cost = colva(times, held, safe, safe, 0.0025);
        EXPECT_LT(cost, 0.0);
        EXPECT_DOUBLE_EQ(colva(times, held, safe, safe, -0.0025), -cost);
        EXPECT_EQ(colva(times, held, safe, safe, 0.0), 0.0);

        // No collateral agreement, no collateral, no ColVA.
        Csa none = Csa::uncollateralised();
        none.margin_period_of_risk = kMpor;
        for (const Real c : discounted_expected_collateral(paths, none))
            EXPECT_EQ(c, 0.0);
        // The receiver's side posts what the payer's side holds.
        HullWhiteExposureEngine mirror(m);
        mirror.add(swap, -1.0);
        const std::vector<Real> posted = discounted_expected_collateral(mirror.simulate(s), csa);
        for (std::size_t r = 0; r < times.size(); r += 7)
            EXPECT_NEAR(posted[r], -held[r], 1e-9 * std::abs(held[r]) + 1e-12);
    }

} // namespace quantModeling
