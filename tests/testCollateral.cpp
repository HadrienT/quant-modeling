#include <gtest/gtest.h>

#include "quantModeling/engines/analytic/swap.hpp"
#include "quantModeling/engines/xva/exposure_engine.hpp"
#include "quantModeling/market/csa.hpp"
#include "quantModeling/risk/collateral.hpp"
#include "quantModeling/risk/exposure_metrics.hpp"
#include "quantModeling/risk/exposure_paths.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

// Lot X2 of blueprint/wp/23-xva.md: collateral as a post-processing of the
// simulated values (ADR-X2). Properties of §15.

namespace quantModeling
{
    namespace
    {
        constexpr Real kEps = 1e-9;
        constexpr Time kTenDays = 10.0 / 250.0;
        const Real kInfinity = std::numeric_limits<Real>::infinity();

        /// Two paths, one trade, five dates: with a margin period of 0.04 the
        /// reporting dates are 0.04 (lagged date: today), 0.54 (0.5) and
        /// 1.04 (1.0). Value today: 2.
        ExposurePaths small_cube()
        {
            ExposurePaths cube;
            cube.times = {0.04, 0.5, 0.54, 1.0, 1.04};
            cube.discount = {1.0, 1.0, 1.0, 1.0, 1.0};
            cube.paths = 2;
            cube.discount_weight.assign(10, 1.0);
            cube.trade_values_today = {2.0};
            cube.trade_values = {{3.0, 5.0, 4.0, -1.0, -6.0, /* path 1 */ 2.0, 2.0, 2.0, 2.0, 2.0}};
            // Path 0 receives 2 at 0.54 and pays 3 at 1.04; the flow of 10 at
            // the lagged date 0.5 came before the last margin call.
            cube.trade_cashflows = {{0.0, 10.0, 2.0, 0.0, -3.0, /* path 1 */ 0.0, 0.0, 0.0, 0.0, 0.0}};
            return cube;
        }

        Csa csa(Real threshold_counterparty, Real threshold_bank, Real mta = 0.0,
                Time mpor = 0.04)
        {
            Csa c;
            c.threshold_counterparty = threshold_counterparty;
            c.threshold_bank = threshold_bank;
            c.minimum_transfer_amount = mta;
            c.margin_period_of_risk = mpor;
            return c;
        }

        /// The collateralised values of path 0 at the three reporting dates.
        std::vector<Real> path0(const ExposurePaths &collateralised)
        {
            return {collateralised.trade_values[0][0], collateralised.trade_values[0][1],
                    collateralised.trade_values[0][2]};
        }

        void expect_values(const std::vector<Real> &actual, const std::vector<Real> &expected)
        {
            ASSERT_EQ(actual.size(), expected.size());
            for (std::size_t i = 0; i < actual.size(); ++i)
                EXPECT_NEAR(actual[i], expected[i], 1e-14) << "reporting date " << i;
        }

        HullWhiteCurveModel model()
        {
            return HullWhiteCurveModel(0.03, 0.01, DiscountCurve(0.03));
        }

        /// A ten-year payer swap at par, notional 100: the bank pays the
        /// fixed coupon once a year and receives the floating one quarterly.
        InterestRateSwap par_payer(const HullWhiteCurveModel &m, Time tenor = 10.0)
        {
            const MultiCurve curves{m.discount(), m.projection()};
            const Real par = value_swap(make_swap(0.0, tenor, 0.0), curves).par_rate;
            return make_swap(0.0, tenor, par, 1, 4, 100.0, true);
        }

        ExposurePaths simulate(const HullWhiteCurveModel &m, Time mpor, std::size_t paths = 10000,
                               Time tenor = 10.0)
        {
            HullWhiteExposureEngine engine(m);
            engine.add(par_payer(m, tenor));
            ExposureSimulationSettings settings;
            settings.paths = paths;
            settings.keep_cashflows = true;
            settings.grid.margin_period_of_risk = mpor;
            return engine.simulate(settings);
        }

        std::size_t index_of(const std::vector<Time> &grid, Time t)
        {
            for (std::size_t i = 0; i < grid.size(); ++i)
                if (std::abs(grid[i] - t) < kEps)
                    return i;
            ADD_FAILURE() << "date " << t << " is not on the grid";
            return 0;
        }

        /// EPE of the discounted profile: one number for "how much exposure".
        Real discounted_epe(const ExposurePaths &cube)
        {
            const ExposureStatistics s = exposure_statistics(cube);
            return expected_positive_exposure(s.times, s.discounted_ee);
        }
    } // namespace

    // ── The CSA formulas (blueprint §4.3) ────────────────────────────────────

    TEST(Csa, RequiredVariationMargin)
    {
        const Csa c = csa(10.0, 5.0);
        EXPECT_DOUBLE_EQ(required_variation_margin(25.0, c), 15.0); // above the threshold
        EXPECT_DOUBLE_EQ(required_variation_margin(7.0, c), 0.0);   // below it: nothing
        EXPECT_DOUBLE_EQ(required_variation_margin(-3.0, c), 0.0);
        EXPECT_DOUBLE_EQ(required_variation_margin(-12.0, c), -7.0); // the bank posts
        // Zero thresholds: the collateral is the value.
        for (const Real v : {-4.0, 0.0, 9.5})
            EXPECT_DOUBLE_EQ(required_variation_margin(v, csa(0.0, 0.0)), v);
        // One-way CSA in the bank's favour: it never posts.
        EXPECT_DOUBLE_EQ(required_variation_margin(-12.0, csa(0.0, kInfinity)), 0.0);
        EXPECT_DOUBLE_EQ(required_variation_margin(1e12, Csa::uncollateralised()), 0.0);
        EXPECT_DOUBLE_EQ(required_variation_margin(-1e12, Csa::uncollateralised()), 0.0);
        Csa bad;
        bad.minimum_transfer_amount = -1.0;
        EXPECT_THROW(bad.validate(), InvalidInput);
    }

    TEST(Csa, MarginCallRespectsTheMinimumTransferAmountAndTheRounding)
    {
        Csa c = csa(0.0, 0.0, 0.5);
        EXPECT_DOUBLE_EQ(collateral_after_call(10.0, 10.4, c), 10.0); // below the MTA
        EXPECT_DOUBLE_EQ(collateral_after_call(10.0, 9.7, c), 10.0);
        EXPECT_DOUBLE_EQ(collateral_after_call(10.0, 10.5, c), 10.5); // at the MTA: called
        EXPECT_DOUBLE_EQ(collateral_after_call(10.0, 3.0, c), 3.0);   // a return
        EXPECT_DOUBLE_EQ(collateral_after_call(10.0, -2.0, c), -2.0); // the bank now posts
        c.rounding = 1.0;
        EXPECT_DOUBLE_EQ(collateral_after_call(10.0, 11.3, c), 11.0);
        EXPECT_DOUBLE_EQ(collateral_after_call(10.0, 11.6, c), 12.0);
        EXPECT_DOUBLE_EQ(collateral_after_call(10.0, 7.4, c), 7.0);
    }

    // ── The collateral recursion on a hand-made cube ─────────────────────────

    TEST(Collateralise, ReportsOnlyTheDatesWhoseLaggedDateIsOnTheGrid)
    {
        const ExposurePaths cube = small_cube();
        const std::vector<std::size_t> reporting = collateral_reporting_dates(cube, 0.04);
        EXPECT_EQ(reporting, (std::vector<std::size_t>{0, 2, 4}));
        // An instantaneous CSA needs no lagged date.
        EXPECT_EQ(collateral_reporting_dates(cube, 0.0).size(), 5u);
        // With a margin period of a quarter only the first date reports (its
        // lagged date is today): the grid was not built for that period.
        EXPECT_EQ(collateral_reporting_dates(cube, 0.25), (std::vector<std::size_t>{0}));
        EXPECT_THROW(collateralise(cube, csa(0.0, 0.0, 0.0, 0.25)), InvalidInput);

        const ExposurePaths collateralised = collateralise(cube, csa(0.0, 0.0));
        EXPECT_EQ(collateralised.times, (std::vector<Time>{0.04, 0.54, 1.04}));
        EXPECT_EQ(collateralised.trades(), 1u);
        EXPECT_EQ(collateralised.paths, 2u);
    }

    TEST(Collateralise, ExposureIsTheValueLessTheCollateralOfTheLaggedDate)
    {
        const ExposurePaths cube = small_cube();
        // Zero thresholds: E(t) = V(t) - V(t - MPoR). The first lagged date
        // is today, where the value is 2.
        expect_values(path0(collateralise(cube, csa(0.0, 0.0))), {3.0 - 2.0, 4.0 - 5.0, -6.0 + 1.0});
        // A threshold of 3 for the counterparty: only the excess is posted.
        expect_values(path0(collateralise(cube, csa(3.0, 0.0))), {3.0 - 0.0, 4.0 - 2.0, -6.0 + 1.0});
        // No collateral at all gives the values back.
        expect_values(path0(collateralise(cube, Csa::uncollateralised())), {3.0, 4.0, -6.0});
        // A flat path leaves nothing uncovered.
        const ExposurePaths flat = collateralise(cube, csa(0.0, 0.0));
        for (std::size_t r = 0; r < 3; ++r)
            EXPECT_DOUBLE_EQ(flat.trade_values[0][3 + r], 0.0);
        // The independent amount is held on top of the variation margin.
        Csa with_ia = csa(0.0, 0.0);
        with_ia.independent_amount = 1.0;
        expect_values(path0(collateralise(cube, with_ia)), {0.0, -2.0, -6.0});
        EXPECT_DOUBLE_EQ(collateralise(cube, with_ia).trade_values_today[0], -1.0);
    }

    TEST(Collateralise, MinimumTransferAmountMakesTheBalancePathDependent)
    {
        const ExposurePaths cube = small_cube();
        // MTA 4: today's call for 2 is not made; at 0.5 the call for 5 is; at
        // 1.0 the return of 6 is.
        expect_values(path0(collateralise(cube, csa(0.0, 0.0, 4.0))), {3.0 - 0.0, 4.0 - 5.0, -6.0 + 1.0});
        // MTA 5.5: the 5 at 0.5 is not called, so at 1.0 the balance is still
        // zero and the required -1 is not transferred either.
        expect_values(path0(collateralise(cube, csa(0.0, 0.0, 5.5))), {3.0, 4.0, -6.0});
    }

    TEST(Collateralise, CashFlowsOfTheMarginPeriod)
    {
        const ExposurePaths cube = small_cube();
        CollateralSettings settings;
        // Classical-: flows are paid, the values already exclude them.
        expect_values(path0(collateralise(cube, csa(0.0, 0.0), {}, settings)), {1.0, -1.0, -5.0});
        // Classical+: nothing is paid in (t - MPoR, t]. The 2 the bank should
        // have received and the 3 it should have paid stay in the close-out;
        // the 10 paid at the lagged date itself does not.
        settings.cashflows = MarginPeriodCashflows::Withheld;
        expect_values(path0(collateralise(cube, csa(0.0, 0.0), {}, settings)), {1.0, -1.0 + 2.0, -5.0 - 3.0});
        // The bank pays, the counterparty does not.
        settings.cashflows = MarginPeriodCashflows::OnlyBankPays;
        expect_values(path0(collateralise(cube, csa(0.0, 0.0), {}, settings)), {1.0, -1.0 + 2.0, -5.0});

        ExposurePaths without_flows = cube;
        without_flows.trade_cashflows.clear();
        EXPECT_THROW(collateralise(without_flows, csa(0.0, 0.0), {}, settings), InvalidInput);
    }

    TEST(Collateralise, InitialMarginCutsEachSidesExposure)
    {
        const ExposurePaths cube = small_cube();
        CollateralSettings settings;
        settings.initial_margin_received = {0.5, 0.5, 0.5};
        settings.initial_margin_posted = {2.0, 2.0, 2.0};
        // 1 -> 0.5 left for the bank; -1 is covered by what the bank posted;
        // -5 -> 3 left for the counterparty.
        expect_values(path0(collateralise(cube, csa(0.0, 0.0), {}, settings)), {0.5, 0.0, -3.0});
        settings.initial_margin_received = {0.5};
        EXPECT_THROW(collateralise(cube, csa(0.0, 0.0), {}, settings), InvalidInput);
        settings.initial_margin_received = {0.5, -0.5, 0.5};
        EXPECT_THROW(collateralise(cube, csa(0.0, 0.0), {}, settings), InvalidInput);
    }

    TEST(Collateralise, DeterministicInitialMarginDecaysWithTheRemainingLife)
    {
        const std::vector<Real> im = deterministic_initial_margin(8.0, {0.0, 3.0, 4.0, 5.0}, 4.0);
        EXPECT_DOUBLE_EQ(im[0], 8.0);
        EXPECT_DOUBLE_EQ(im[1], 4.0); // sqrt(1/4)
        EXPECT_DOUBLE_EQ(im[2], 0.0);
        EXPECT_DOUBLE_EQ(im[3], 0.0);
        EXPECT_THROW(deterministic_initial_margin(-1.0, {1.0}, 4.0), InvalidInput);
    }

    // ── On simulated swap exposures ──────────────────────────────────────────

    TEST(CollateralisedExposure, TheGridCarriesTheLaggedDates)
    {
        const HullWhiteCurveModel m = model();
        HullWhiteExposureEngine engine(m);
        engine.add(par_payer(m));
        ExposureGridSettings with_lags;
        with_lags.margin_period_of_risk = kTenDays;
        const std::vector<Time> base = engine.grid();
        const std::vector<Time> grid = engine.grid(with_lags);
        for (const Time t : base)
        {
            EXPECT_TRUE(std::binary_search(grid.begin(), grid.end(), t)) << t;
            if (t - kTenDays > kEps)
                EXPECT_TRUE(std::any_of(grid.begin(), grid.end(),
                                        [t](Time g)
                                        { return std::abs(g - (t - kTenDays)) < kEps; }))
                    << t;
        }
        // Every date of the base grid becomes a reporting date.
        const ExposurePaths cube = simulate(m, kTenDays, 16);
        const ExposurePaths collateralised = collateralise(cube, csa(0.0, 0.0, 0.0, kTenDays));
        for (const Time t : base)
            EXPECT_TRUE(std::any_of(collateralised.times.begin(), collateralised.times.end(),
                                    [t](Time g)
                                    { return std::abs(g - t) < kEps; }))
                << t;
        // Without lagged dates on the grid there is nothing to report.
        EXPECT_THROW(collateralise(simulate(m, 0.0, 16), csa(0.0, 0.0, 0.0, kTenDays)), InvalidInput);
    }

    TEST(CollateralisedExposure, InfiniteThresholdsAreNoCsaBitForBit)
    {
        const HullWhiteCurveModel m = model();
        const ExposurePaths cube = simulate(m, kTenDays, 500);
        Csa none = Csa::uncollateralised();
        none.margin_period_of_risk = kTenDays;
        const ExposurePaths collateralised = collateralise(cube, none);
        const std::vector<std::size_t> reporting = collateral_reporting_dates(cube, kTenDays);
        const std::size_t n = cube.dates(), reported = reporting.size();
        for (std::size_t p = 0; p < cube.paths; ++p)
            for (std::size_t r = 0; r < reported; ++r)
            {
                ASSERT_EQ(collateralised.trade_values[0][p * reported + r],
                          cube.trade_values[0][p * n + reporting[r]]);
                ASSERT_EQ(collateralised.discount_weight[p * reported + r],
                          cube.discount_weight[p * n + reporting[r]]);
            }
    }

    TEST(CollateralisedExposure, ResidualExposureIsTheMoveOverTheMarginPeriod)
    {
        // Zero threshold, flows withheld: E(t) = V(t) - V(t - MPoR) plus the
        // flows of the period, a nearly Gaussian move. Its expected positive
        // part is the normal formula of lot X0: 0.4 σ for a centred move.
        const HullWhiteCurveModel m = model();
        const ExposurePaths cube = simulate(m, kTenDays, 40000);
        CollateralSettings withheld;
        withheld.cashflows = MarginPeriodCashflows::Withheld;
        const Csa zero = csa(0.0, 0.0, 0.0, kTenDays);
        const ExposurePaths collateralised = collateralise(cube, zero, {}, withheld);
        const ExposureStatistics s = exposure_statistics(collateralised);
        const ExposureStatistics uncollateralised = exposure_statistics(cube);

        for (const Time t : {2.0, 5.0, 8.0})
        {
            const std::size_t r = index_of(collateralised.times, t);
            const std::size_t dates = collateralised.dates();
            Real w_sum = 0.0, mean = 0.0, second = 0.0;
            for (std::size_t p = 0; p < collateralised.paths; ++p)
            {
                const Real w = collateralised.discount_weight[p * dates + r];
                const Real move = collateralised.trade_values[0][p * dates + r];
                w_sum += w;
                mean += w * move;
                second += w * move * move;
            }
            mean /= w_sum;
            const Real sd = std::sqrt(second / w_sum - mean * mean);
            EXPECT_NEAR(s.ee[r] / normal_expected_exposure(mean, sd), 1.0, 0.03) << "t=" << t;
            // The move over ten days is small against its volatility.
            EXPECT_NEAR(s.ee[r] / (0.3989422804 * sd), 1.0, 0.10) << "t=" << t;
            // An order of magnitude below the uncollateralised exposure.
            EXPECT_LT(s.ee[r], 0.2 * uncollateralised.ee[index_of(uncollateralised.times, t)]);
        }
    }

    TEST(CollateralisedExposure, ScalesLikeTheSquareRootOfTheMarginPeriod)
    {
        // 5, 10 and 20 business days: each doubling multiplies the residual
        // exposure by sqrt(2). (Separate simulations: the lagged dates differ.)
        const HullWhiteCurveModel m = model();
        CollateralSettings withheld;
        withheld.cashflows = MarginPeriodCashflows::Withheld;
        std::vector<Real> epe;
        for (const Time mpor : {5.0 / 250.0, 10.0 / 250.0, 20.0 / 250.0})
            epe.push_back(discounted_epe(
                collateralise(simulate(m, mpor, 20000), csa(0.0, 0.0, 0.0, mpor), {}, withheld)));
        EXPECT_NEAR(epe[1] / epe[0], std::sqrt(2.0), 0.08);
        EXPECT_NEAR(epe[2] / epe[1], std::sqrt(2.0), 0.08);
    }

    TEST(CollateralisedExposure, GrowsWithTheThresholdPathByPath)
    {
        // Raising the counterparty's threshold only removes collateral held:
        // V - C rises on every path, so does every exposure statistic.
        const HullWhiteCurveModel m = model();
        const ExposurePaths cube = simulate(m, kTenDays, 4000);
        std::vector<ExposureStatistics> stats;
        for (const Real threshold : {0.0, 1.0, 3.0, 10.0, kInfinity})
            stats.push_back(exposure_statistics(collateralise(cube, csa(threshold, 0.0, 0.0, kTenDays))));
        for (std::size_t k = 1; k < stats.size(); ++k)
            for (std::size_t i = 0; i < stats[k].times.size(); ++i)
            {
                EXPECT_GE(stats[k].discounted_ee[i], stats[k - 1].discounted_ee[i] - 1e-14);
                EXPECT_GE(stats[k].pfe[i], stats[k - 1].pfe[i] - 1e-14);
            }
        // A one-way CSA (the bank never posts) holds at least as much
        // collateral as the two-way one: lower exposure, on both sides.
        const ExposureStatistics one_way =
            exposure_statistics(collateralise(cube, csa(0.0, kInfinity, 0.0, kTenDays)));
        for (std::size_t i = 0; i < one_way.times.size(); ++i)
        {
            EXPECT_LE(one_way.discounted_ee[i], stats[0].discounted_ee[i] + 1e-14);
            EXPECT_LE(one_way.discounted_ene[i], stats[0].discounted_ene[i] + 1e-14);
        }
    }

    TEST(CollateralisedExposure, GrowsWithTheMinimumTransferAmount)
    {
        // Not path by path — an uncalled amount can fall on either side — but
        // on average each uncalled amount is exposure left uncovered.
        const HullWhiteCurveModel m = model();
        const ExposurePaths cube = simulate(m, kTenDays, 10000);
        Real previous = 0.0;
        for (const Real mta : {0.0, 0.5, 1.0, 2.0, 5.0})
        {
            const Real epe = discounted_epe(collateralise(cube, csa(0.0, 0.0, mta, kTenDays)));
            EXPECT_GT(epe, previous) << "MTA=" << mta;
            previous = epe;
        }
        // And any CSA beats none.
        Csa none = Csa::uncollateralised();
        none.margin_period_of_risk = kTenDays;
        EXPECT_LT(previous, discounted_epe(collateralise(cube, none)));
    }

    TEST(CollateralisedExposure, SpikesWhereTheBankPaysACoupon)
    {
        // At each anniversary the bank pays the fixed coupon (about 3) and
        // receives a quarter of floating: the value jumps up by the net
        // payment while the collateral still reflects the value before it.
        const HullWhiteCurveModel m = model();
        const ExposurePaths cube = simulate(m, kTenDays, 10000);
        const Csa zero = csa(0.0, 0.0, 0.0, kTenDays);
        CollateralSettings settings;

        const ExposureStatistics paid = exposure_statistics(collateralise(cube, zero, {}, settings));
        settings.cashflows = MarginPeriodCashflows::Withheld;
        const ExposureStatistics withheld = exposure_statistics(collateralise(cube, zero, {}, settings));
        settings.cashflows = MarginPeriodCashflows::OnlyBankPays;
        const ExposureStatistics adverse = exposure_statistics(collateralise(cube, zero, {}, settings));

        for (const Time anniversary : {1.0, 3.0, 6.0})
        {
            const std::size_t at = index_of(paid.times, anniversary);
            const std::size_t before = index_of(paid.times, anniversary - 0.25);
            // Classical-: a spike several times the diffusive exposure...
            EXPECT_GT(paid.ee[at], 4.0 * withheld.ee[at]) << anniversary;
            // ...of the size of the net coupon paid.
            EXPECT_GT(paid.ee[at], 1.5) << anniversary;
            EXPECT_LT(paid.ee[at], 3.5) << anniversary;
            // Classical+: no spike, the anniversary looks like any other date.
            EXPECT_NEAR(withheld.ee[at] / withheld.ee[before], 1.0, 0.35) << anniversary;
        }
        // The adverse case dominates both, on every date.
        for (std::size_t i = 0; i < paid.times.size(); ++i)
        {
            EXPECT_GE(adverse.discounted_ee[i], paid.discounted_ee[i] - 1e-14);
            EXPECT_GE(adverse.discounted_ee[i], withheld.discounted_ee[i] - 1e-14);
        }
        // Where the bank receives a floating coupon it is the counterparty
        // that is exposed, not the bank.
        const std::size_t quarter = index_of(paid.times, 1.25);
        EXPECT_LT(paid.ee[quarter], withheld.ee[quarter]);
        EXPECT_LT(paid.ene[quarter], withheld.ene[quarter]);
    }

    TEST(CollateralisedExposure, InitialMarginAt99PercentLeavesOnePathInAHundredExposed)
    {
        // An initial margin equal to the 99 % quantile of the move over the
        // margin period (blueprint §5.5): P(E > 0) <= 1 % by construction and
        // the expected exposure collapses.
        const HullWhiteCurveModel m = model();
        const ExposurePaths cube = simulate(m, kTenDays, 20000);
        const Csa zero = csa(0.0, 0.0, 0.0, kTenDays);
        CollateralSettings settings;
        settings.cashflows = MarginPeriodCashflows::Withheld;
        const ExposureStatistics without = exposure_statistics(collateralise(cube, zero, {}, settings), {}, 0.99);

        settings.initial_margin_received = without.pfe;
        const ExposurePaths with_im = collateralise(cube, zero, {}, settings);
        const ExposureStatistics with = exposure_statistics(with_im);

        const std::size_t dates = with_im.dates();
        for (const Time t : {1.0, 4.0, 9.0})
        {
            const std::size_t r = index_of(with_im.times, t);
            std::size_t exposed = 0;
            for (std::size_t p = 0; p < with_im.paths; ++p)
                if (with_im.trade_values[0][p * dates + r] > 0.0)
                    ++exposed;
            EXPECT_LE(static_cast<Real>(exposed) / static_cast<Real>(with_im.paths), 0.0115) << t;
            EXPECT_LT(with.ee[r], without.ee[r] / 20.0) << t;
            // The margin the bank received does nothing for the counterparty.
            EXPECT_DOUBLE_EQ(with.discounted_ene[r], without.discounted_ene[r]);
        }
    }

} // namespace quantModeling
