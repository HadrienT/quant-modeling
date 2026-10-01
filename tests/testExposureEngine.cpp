#include <gtest/gtest.h>

#include "quantModeling/engines/analytic/hull_white_swaption.hpp"
#include "quantModeling/engines/analytic/swap.hpp"
#include "quantModeling/engines/xva/exposure_engine.hpp"
#include "quantModeling/engines/xva/hull_white_future_value.hpp"
#include "quantModeling/market/credit_curve.hpp"
#include "quantModeling/market/curve_bootstrap.hpp"
#include "quantModeling/market/multi_curve_bootstrap.hpp"
#include "quantModeling/risk/exposure_metrics.hpp"
#include "quantModeling/risk/exposure_paths.hpp"
#include "quantModeling/risk/xva.hpp"
#include "quantModeling/utils/stats.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

// Lot X1 of blueprint/wp/23-xva.md: the exposure engine under Hull-White.
// The properties tested are those of §15 of the work package.

namespace quantModeling
{
    namespace
    {
        constexpr Real kEps = 1e-10;

        /// An upward-sloping OIS curve and a three-month projection curve
        /// above it, as in tests/testRates.cpp: a real multi-curve set-up.
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

        /// A spot-starting swap struck at par: worth zero today.
        InterestRateSwap par_swap(const HullWhiteCurveModel &m, Time tenor, bool payer,
                                  Real notional = 100.0)
        {
            const MultiCurve curves{m.discount(), m.projection()};
            const Real par = value_swap(make_swap(0.0, tenor, 0.0), curves).par_rate;
            return make_swap(0.0, tenor, par, 1, 4, notional, payer);
        }

        std::size_t index_of(const std::vector<Time> &grid, Time t)
        {
            for (std::size_t i = 0; i < grid.size(); ++i)
                if (std::abs(grid[i] - t) < kEps)
                    return i;
            ADD_FAILURE() << "date " << t << " is not on the grid";
            return 0;
        }

        /// Value of a swap at grid[i] on one path, straight from the
        /// definition and the model's zero-coupon bond — no precomputation.
        Real swap_value_by_definition(const InterestRateSwap &swap, const HullWhiteCurveModel &m,
                                      const std::vector<Time> &grid, std::size_t i,
                                      const std::vector<Real> &state)
        {
            const Time t = grid[i];
            const Real x = state[i];
            const Real side = swap.payer ? 1.0 : -1.0;
            Real v = 0.0;
            for (const CouponPeriod &c : swap.fixed_leg)
                if (c.payment > t + kEps)
                    v -= side * swap.notional * swap.fixed_rate * c.accrual * m.zcb(t, c.payment, x);
            for (const CouponPeriod &c : swap.floating_leg)
            {
                if (c.end <= t + kEps)
                    continue;
                const Real beta = (m.projection().discount(c.start) / m.projection().discount(c.end)) /
                                  (m.discount().discount(c.start) / m.discount().discount(c.end));
                Real libor; // the rate L of the coupon as seen at t
                if (c.start >= t - kEps)
                    libor = (beta * m.zcb(t, c.start, x) / m.zcb(t, c.end, x) - 1.0) / c.accrual;
                else
                {
                    const Real x_fixing = c.start < kEps ? 0.0 : state[index_of(grid, c.start)];
                    libor = (beta / m.zcb(c.start, c.end, x_fixing) - 1.0) / c.accrual;
                }
                v += side * swap.notional * (libor + swap.spread) * c.accrual * m.zcb(t, c.end, x);
            }
            return v;
        }

        /// Present value today of the coupons paid strictly after t: by the
        /// martingale property this is EFV*(t).
        Real swap_tail_value_today(const InterestRateSwap &swap, const HullWhiteCurveModel &m, Time t)
        {
            const Real side = swap.payer ? 1.0 : -1.0;
            Real v = 0.0;
            for (const CouponPeriod &c : swap.fixed_leg)
                if (c.payment > t + kEps)
                    v -= side * swap.notional * swap.fixed_rate * c.accrual * m.discount().discount(c.payment);
            for (const CouponPeriod &c : swap.floating_leg)
                if (c.payment > t + kEps)
                    v += side * swap.notional *
                         (forward_rate(m.projection(), c.start, c.end, c.accrual) + swap.spread) *
                         c.accrual * m.discount().discount(c.payment);
            return v;
        }
    } // namespace

    // ── Grid ─────────────────────────────────────────────────────────────────

    TEST(ExposureGrid, ThinsOutWithTimeAndContainsEveryEventDate)
    {
        const std::vector<Time> events = {0.0, 0.37, 1.5, 4.123, 12.0};
        const std::vector<Time> grid = exposure_grid(10.0, events);
        ASSERT_FALSE(grid.empty());
        EXPECT_GT(grid.front(), 0.0); // today is not on the grid
        EXPECT_NEAR(grid.front(), 1.0 / 52.0, 1e-15);
        EXPECT_NEAR(grid.back(), 10.0, 1e-15);
        for (std::size_t i = 1; i < grid.size(); ++i)
            EXPECT_GT(grid[i], grid[i - 1] + kEps);
        for (const Time t : {0.37, 1.5, 4.123})
            EXPECT_TRUE(std::any_of(grid.begin(), grid.end(),
                                    [t](Time g)
                                    { return std::abs(g - t) < kEps; }))
                << t;
        // Steps: at most a week in the first month, a month up to two years,
        // a quarter beyond.
        for (std::size_t i = 1; i < grid.size(); ++i)
        {
            const Time step = grid[i] - grid[i - 1];
            const Time allowed = grid[i] <= 1.0 / 12.0 + kEps ? 1.0 / 52.0
                                 : grid[i] <= 2.0 + kEps      ? 1.0 / 12.0
                                                              : 0.25;
            EXPECT_LE(step, allowed + kEps) << "at t=" << grid[i];
        }
        EXPECT_THROW(exposure_grid(0.0, {}), InvalidInput);
    }

    // ── Future values (ADR-X1: exact repricing) ──────────────────────────────

    TEST(HullWhiteFutureValue, SwapMatchesItsDefinitionOnAnArbitraryPath)
    {
        const HullWhiteCurveModel m = model();
        for (const bool payer : {true, false})
        {
            // Off-market, with a spread on the floating leg, semi-annual fixed.
            const InterestRateSwap swap = make_swap(0.0, 5.0, 0.045, 2, 4, 250.0, payer, 0.0015);
            const std::unique_ptr<FutureValue> value = make_future_value(swap, m);
            const std::vector<Time> grid = exposure_grid(value->maturity(), value->event_times());
            value->bind(grid);

            std::vector<Real> state(grid.size());
            for (std::size_t i = 0; i < grid.size(); ++i)
                state[i] = 0.02 * std::sin(1.7 * static_cast<Real>(i)) + 0.004 * grid[i];

            for (std::size_t i = 0; i < grid.size(); ++i)
                EXPECT_NEAR(value->value(i, state.data()),
                            swap_value_by_definition(swap, m, grid, i, state), 1e-11)
                    << "t=" << grid[i];
            // Nothing is left after the last payment.
            EXPECT_DOUBLE_EQ(value->value(grid.size() - 1, state.data()), 0.0);

            // Today's value is the multi-curve swap price.
            EXPECT_NEAR(value->value_today(),
                        value_swap(swap, MultiCurve{m.discount(), m.projection()}).npv, 1e-12);
        }
    }

    TEST(HullWhiteFutureValue, SwapCashflowsAreTheCouponsFixedOnThePath)
    {
        const HullWhiteCurveModel m = model();
        const InterestRateSwap swap = make_swap(0.0, 2.0, 0.04, 1, 4, 100.0, true);
        const std::unique_ptr<FutureValue> value = make_future_value(swap, m);
        const std::vector<Time> grid = exposure_grid(value->maturity(), value->event_times());
        value->bind(grid);
        std::vector<Real> state(grid.size());
        for (std::size_t i = 0; i < grid.size(); ++i)
            state[i] = 0.01 * std::cos(static_cast<Real>(i));

        Real floating_count = 0.0;
        for (std::size_t i = 0; i < grid.size(); ++i)
        {
            Real expected = 0.0;
            for (const CouponPeriod &c : swap.fixed_leg)
                if (std::abs(c.payment - grid[i]) < kEps)
                    expected -= 100.0 * 0.04 * c.accrual;
            for (const CouponPeriod &c : swap.floating_leg)
                if (std::abs(c.end - grid[i]) < kEps)
                {
                    const Real beta = (m.projection().discount(c.start) / m.projection().discount(c.end)) /
                                      (m.discount().discount(c.start) / m.discount().discount(c.end));
                    const Real x = c.start < kEps ? 0.0 : state[index_of(grid, c.start)];
                    expected += 100.0 * (beta / m.zcb(c.start, c.end, x) - 1.0);
                    floating_count += 1.0;
                }
            EXPECT_NEAR(value->cashflow(i, state.data()), expected, 1e-12) << "t=" << grid[i];
        }
        EXPECT_DOUBLE_EQ(floating_count, 8.0);
        // The first coupon is fixed today, off the projection curve.
        EXPECT_NEAR(value->cashflow(index_of(grid, 0.25), state.data()),
                    100.0 * 0.25 * forward_rate(m.projection(), 0.0, 0.25, 0.25), 1e-12);
    }

    TEST(HullWhiteFutureValue, SwaptionBeforeExpiryMatchesQuadrature)
    {
        const HullWhiteCurveModel m = model();
        const Time expiry = 3.0;
        const Swaption swaption{make_swap(expiry, 7.0, 0.04, 1, 4, 100.0, true), expiry};
        const HullWhiteExerciseRegion region = hull_white_exercise_region(swaption, m);
        const std::vector<BondCashflow> bonds = swap_as_bonds(swaption.swap, expiry, m);

        for (const Time t : {0.5, 1.0, 2.75})
            for (const Real x : {-0.015, 0.0, 0.02})
            {
                // E^T[max(swap(T), 0) | x(t)] by the trapezoid rule on the
                // Gaussian transition of x.
                const auto tr = m.transition(t, expiry, expiry);
                const Real mean = tr.decay * x + tr.drift, sd = std::sqrt(tr.variance);
                const int n = 8000;
                Real sum = 0.0;
                for (int i = 0; i <= n; ++i)
                {
                    const Real z = -10.0 + 20.0 * i / n;
                    const Real xT = mean + sd * z;
                    Real payoff = 0.0;
                    for (const BondCashflow &c : bonds)
                        payoff += c.amount * m.zcb(expiry, c.time, xT);
                    sum += ((i == 0 || i == n) ? 0.5 : 1.0) * norm_pdf(z) * std::max(payoff, 0.0);
                }
                const Real quadrature = m.zcb(t, expiry, x) * sum * 20.0 / n;
                EXPECT_NEAR(hull_white_european_swaption(region, m, t, x), quadrature,
                            2e-6 * std::max(quadrature, 1.0))
                    << "t=" << t << " x=" << x;
            }
        // Seen from today it is the closed form already validated in testRates.
        EXPECT_NEAR(hull_white_european_swaption(region, m, 0.0, 0.0),
                    hull_white_european_swaption(swaption, m), 1e-14);
        // At the expiry it is the exercise value.
        Real exercise = 0.0;
        for (const BondCashflow &c : bonds)
            exercise += c.amount * m.zcb(expiry, c.time, 0.01);
        EXPECT_NEAR(hull_white_european_swaption(region, m, expiry, 0.01), std::max(exercise, 0.0),
                    1e-12);
    }

    TEST(HullWhiteFutureValue, RejectsWhatTheModelCannotRepriceInClosedForm)
    {
        const HullWhiteCurveModel m = model();
        HullWhiteExposureEngine engine(m);
        const BermudanSwaption bermudan{make_swap(1.0, 5.0, 0.04), {1.0, 2.0, 3.0}};
        EXPECT_THROW(engine.add(bermudan), UnsupportedInstrument);
        // A swap whose first floating period is under way needs a past fixing.
        InterestRateSwap seasoned = make_swap(0.0, 2.0, 0.04);
        seasoned.floating_leg.front().start = -0.1;
        EXPECT_THROW(engine.add(seasoned), InvalidInput);
        EXPECT_THROW(engine.simulate(), InvalidInput); // no trade
    }

    // ── Simulation ───────────────────────────────────────────────────────────

    TEST(ExposureEngine, DiscountWeightsAverageToTodaysDiscountFactors)
    {
        // P(t, T*) / P(0, T*) deflated zero-coupon bonds are martingales:
        // E[w(t)] = P(0, t). This validates the measure and the transitions.
        const HullWhiteCurveModel m = model();
        HullWhiteExposureEngine engine(m);
        engine.add(par_swap(m, 10.0, true));
        ExposureSimulationSettings settings;
        settings.paths = 20000;
        const ExposurePaths paths = engine.simulate(settings);

        const std::size_t n = paths.dates();
        for (std::size_t i = 0; i < n; ++i)
        {
            Real sum = 0.0, sum2 = 0.0;
            for (std::size_t p = 0; p < paths.paths; ++p)
            {
                const Real w = paths.discount_weight[p * n + i];
                sum += w;
                sum2 += w * w;
            }
            const Real N = static_cast<Real>(paths.paths);
            const Real mean = sum / N;
            const Real error = std::sqrt((sum2 / N - mean * mean) / N);
            EXPECT_NEAR(mean, m.discount().discount(paths.times[i]), 3.5 * error + 1e-14)
                << "t=" << paths.times[i];
            EXPECT_NEAR(paths.discount[i], m.discount().discount(paths.times[i]), 1e-15);
        }
        // At the horizon the numeraire is worth one: the weight is deterministic.
        EXPECT_NEAR(paths.discount_weight[n - 1], paths.discount[n - 1], 1e-13);
    }

    TEST(ExposureEngine, ExpectedFutureValueOfASwapIsTheValueOfItsRemainingCoupons)
    {
        // EFV*(t) = E[D(t) V(t)] = value today of the cash flows after t
        // (blueprint §15), within three Monte-Carlo standard errors.
        const HullWhiteCurveModel m = model();
        const InterestRateSwap swap = make_swap(0.0, 10.0, 0.045, 1, 4, 100.0, true, 0.001);
        HullWhiteExposureEngine engine(m);
        engine.add(swap);
        ExposureSimulationSettings settings;
        settings.paths = 20000;
        const ExposureStatistics s = exposure_statistics(engine.simulate(settings));

        std::size_t outside_three_sigma = 0;
        for (std::size_t i = 0; i < s.times.size(); ++i)
        {
            const Real exact = swap_tail_value_today(swap, m, s.times[i]);
            const Real deviation = std::abs(s.discounted_efv[i] - exact);
            EXPECT_LE(deviation, 4.5 * s.discounted_efv_error[i] + 1e-12) << "t=" << s.times[i];
            if (deviation > 3.0 * s.discounted_efv_error[i] + 1e-12)
                ++outside_three_sigma;
        }
        // Three sigma is a 99.7 % band: on ~70 (correlated) dates at most a
        // couple may fall outside.
        EXPECT_LE(outside_three_sigma, 2u);
        EXPECT_NEAR(s.value_today, value_swap(swap, MultiCurve{m.discount(), m.projection()}).npv,
                    1e-12);
    }

    TEST(ExposureEngine, ExpectedExposurePlusNegativeExposureIsTheExpectedFutureValue)
    {
        const HullWhiteCurveModel m = model();
        HullWhiteExposureEngine engine(m);
        engine.add(par_swap(m, 5.0, true));
        engine.add(Swaption{make_swap(2.0, 5.0, 0.04, 1, 4, 100.0, false), 2.0}, -1.0);
        ExposureSimulationSettings settings;
        settings.paths = 4000;
        const ExposureStatistics s = exposure_statistics(engine.simulate(settings));
        for (std::size_t i = 0; i < s.times.size(); ++i)
        {
            EXPECT_NEAR(s.discounted_ee[i] + s.discounted_ene[i], s.discounted_efv[i], 1e-12);
            EXPECT_NEAR(s.ee[i] + s.ene[i], s.efv[i], 1e-12);
            EXPECT_GE(s.discounted_ee[i], 0.0);
            EXPECT_LE(s.discounted_ene[i], 0.0);
            EXPECT_GE(s.pfe[i], 0.0);
            // The undiscounted profile is the discounted one over P(0, t).
            EXPECT_NEAR(s.ee[i] * m.discount().discount(s.times[i]), s.discounted_ee[i], 1e-13);
        }
        // The profile feeds the xVA integrals of lot X0 as it is.
        EXPECT_NO_THROW(s.profile().validate());
    }

    TEST(ExposureEngine, SwapExposureIsAHumpPeakingAroundAThirdOfItsLife)
    {
        // Two opposite effects (blueprint §3.3): rates diffuse like sqrt(t),
        // the remaining duration shrinks like T - t.
        const HullWhiteCurveModel m = model();
        const Time T = 10.0;
        HullWhiteExposureEngine engine(m);
        engine.add(par_swap(m, T, true));
        ExposureSimulationSettings settings;
        settings.paths = 20000;
        const ExposureStatistics s = exposure_statistics(engine.simulate(settings));

        const std::size_t peak = static_cast<std::size_t>(
            std::max_element(s.ee.begin(), s.ee.end()) - s.ee.begin());
        EXPECT_GT(s.times[peak], 0.2 * T);
        EXPECT_LT(s.times[peak], 0.5 * T);
        // Almost nothing in the first week, nothing after the last payment.
        EXPECT_LT(s.ee.front(), 0.2 * s.ee[peak]);
        EXPECT_DOUBLE_EQ(s.ee.back(), 0.0);
        EXPECT_DOUBLE_EQ(s.pfe.back(), 0.0);
        // The PFE envelope sits above the expected exposure at the peak.
        EXPECT_GT(s.pfe[peak], 2.0 * s.ee[peak]);
        // A profile that rolls off: the effective EE keeps its maximum, so
        // over the first year EEPE >= the plain average.
        EXPECT_GT(s.epe, 0.0);
        EXPECT_GT(s.eepe, 0.0);
    }

    TEST(ExposureEngine, SwapValueIsCloseToNormalSoTheClosedFormsOfLotX0Apply)
    {
        // The value of a par swap is nearly linear in the Gaussian state:
        // EE and PFE must agree with the normal formulas fed with the
        // simulated mean and standard deviation (blueprint §3.2: "an engine
        // that does not recover them on a Gaussian product has a bug").
        const HullWhiteCurveModel m = model();
        HullWhiteExposureEngine engine(m);
        engine.add(par_swap(m, 10.0, true));
        ExposureSimulationSettings settings;
        settings.paths = 40000;
        const ExposurePaths paths = engine.simulate(settings);
        const ExposureStatistics s = exposure_statistics(paths, {}, 0.95);

        const std::size_t n = paths.dates();
        const std::size_t i = index_of(paths.times, 3.0);
        // Moments under the t-forward measure: paths weighted by w.
        Real w_sum = 0.0, mean = 0.0, second = 0.0;
        for (std::size_t p = 0; p < paths.paths; ++p)
        {
            const Real w = paths.discount_weight[p * n + i], v = paths.trade_values[0][p * n + i];
            w_sum += w;
            mean += w * v;
            second += w * v * v;
        }
        mean /= w_sum;
        const Real sd = std::sqrt(second / w_sum - mean * mean);
        EXPECT_NEAR(s.ee[i] / normal_expected_exposure(mean, sd), 1.0, 0.02);
        // The quantile shows what the mean hides: bond prices are convex in
        // the state, so the value of a payer swap is bounded above and its
        // upper tail is thinner than a normal one. PFE is below the normal
        // figure, by a few percent.
        const Real pfe_ratio = s.pfe[i] / normal_potential_future_exposure(mean, sd, 0.95);
        EXPECT_LT(pfe_ratio, 1.0);
        EXPECT_GT(pfe_ratio, 0.92);
    }

    TEST(ExposureEngine, BoughtSwaptionHasConstantDiscountedExposureUntilExpiry)
    {
        // The discounted value of an option is a positive martingale:
        // EE*(t) = V_0 and there is no negative exposure (blueprint §7.3).
        const HullWhiteCurveModel m = model();
        const Time expiry = 2.0;
        const Swaption swaption{make_swap(expiry, 5.0, 0.04, 1, 4, 100.0, true), expiry};
        const Real v0 = hull_white_european_swaption(swaption, m);

        HullWhiteExposureEngine engine(m);
        engine.add(swaption);
        ExposureSimulationSettings settings;
        settings.paths = 20000;
        const ExposureStatistics s = exposure_statistics(engine.simulate(settings));
        EXPECT_NEAR(s.value_today, v0, 1e-14);

        bool negative_after_expiry = false;
        for (std::size_t i = 0; i < s.times.size(); ++i)
        {
            if (s.times[i] <= expiry + kEps)
            {
                EXPECT_NEAR(s.discounted_ee[i], v0, 4.0 * s.discounted_ee_error[i] + 1e-12)
                    << "t=" << s.times[i];
                EXPECT_DOUBLE_EQ(s.discounted_ene[i], 0.0);
            }
            else if (s.discounted_ene[i] < 0.0)
                negative_after_expiry = true;
        }
        // Physically settled: once exercised it is a swap, which can turn
        // against its holder.
        EXPECT_TRUE(negative_after_expiry);

        // Hence CVA = -LGD V_0 PD(0, expiry) on the life of the option.
        ExposureProfile until_expiry;
        for (std::size_t i = 0; i < s.times.size() && s.times[i] <= expiry + kEps; ++i)
        {
            until_expiry.times.push_back(s.times[i]);
            until_expiry.discounted_ee.push_back(s.discounted_ee[i]);
            until_expiry.discounted_ene.push_back(s.discounted_ene[i]);
        }
        const CreditCurve counterparty(0.03);
        EXPECT_NEAR(cva_unilateral(until_expiry, counterparty, 0.6),
                    -0.6 * v0 * counterparty.default_probability(expiry), 0.02 * 0.6 * v0 * 0.06);
    }

    TEST(ExposureEngine, SoldSwaptionHasNoPositiveExposureUntilExpiry)
    {
        const HullWhiteCurveModel m = model();
        const Time expiry = 2.0;
        HullWhiteExposureEngine engine(m);
        engine.add(Swaption{make_swap(expiry, 5.0, 0.04, 1, 4, 100.0, true), expiry}, -1.0);
        ExposureSimulationSettings settings;
        settings.paths = 4000;
        const ExposureStatistics s = exposure_statistics(engine.simulate(settings));
        EXPECT_LT(s.value_today, 0.0);
        for (std::size_t i = 0; i < s.times.size() && s.times[i] <= expiry + kEps; ++i)
        {
            EXPECT_DOUBLE_EQ(s.discounted_ee[i], 0.0);
            EXPECT_DOUBLE_EQ(s.pfe[i], 0.0);
            EXPECT_LT(s.discounted_ene[i], 0.0);
        }
    }

    TEST(ExposureEngine, ValuePlusCashFlowsPaidIsAMartingale)
    {
        // E[D(t) V(t) + Σ_{t_j <= t} D(t_j) CF(t_j)] = V_0 at every date: the
        // coupons fixed on the path and the exercise decision of the
        // swaption are consistent with the values.
        const HullWhiteCurveModel m = model();
        HullWhiteExposureEngine engine(m);
        engine.add(make_swap(0.0, 5.0, 0.042, 2, 4, 100.0, false, 0.0005));
        engine.add(Swaption{make_swap(1.0, 4.0, 0.039, 1, 4, 100.0, true), 1.0});
        ExposureSimulationSettings settings;
        settings.paths = 20000;
        settings.keep_cashflows = true;
        const ExposurePaths paths = engine.simulate(settings);
        const Real v0 = paths.trade_values_today[0] + paths.trade_values_today[1];

        const std::size_t n = paths.dates();
        const Real N = static_cast<Real>(paths.paths);
        std::vector<Real> paid(paths.paths, 0.0); // Σ D CF so far, per path
        for (std::size_t i = 0; i < n; ++i)
        {
            Real sum = 0.0, sum2 = 0.0;
            for (std::size_t p = 0; p < paths.paths; ++p)
            {
                const Real w = paths.discount_weight[p * n + i];
                paid[p] += w * (paths.trade_cashflows[0][p * n + i] + paths.trade_cashflows[1][p * n + i]);
                const Real y = paid[p] + w * (paths.trade_values[0][p * n + i] + paths.trade_values[1][p * n + i]);
                sum += y;
                sum2 += y * y;
            }
            const Real mean = sum / N;
            const Real error = std::sqrt((sum2 / N - mean * mean) / N);
            EXPECT_NEAR(mean, v0, 4.0 * error + 1e-12) << "t=" << paths.times[i];
        }
    }

    // ── Netting and allocation ───────────────────────────────────────────────

    TEST(ExposureEngine, NettingNeverIncreasesExposure)
    {
        // max(Σ V_k, 0) <= Σ max(V_k, 0) path by path, on common paths.
        const HullWhiteCurveModel m = model();
        HullWhiteExposureEngine engine(m);
        engine.add(par_swap(m, 10.0, true));
        engine.add(par_swap(m, 5.0, false));
        engine.add(Swaption{make_swap(2.0, 5.0, 0.04, 1, 4, 100.0, true), 2.0});
        ExposureSimulationSettings settings;
        settings.paths = 5000;
        const ExposurePaths paths = engine.simulate(settings);

        const ExposureStatistics netted = exposure_statistics(paths);
        const ExposureStatistics a = exposure_statistics(paths, {0});
        const ExposureStatistics b = exposure_statistics(paths, {1});
        const ExposureStatistics c = exposure_statistics(paths, {2});
        Real netted_area = 0.0, gross_area = 0.0;
        for (std::size_t i = 0; i < netted.times.size(); ++i)
        {
            const Real gross = a.discounted_ee[i] + b.discounted_ee[i] + c.discounted_ee[i];
            EXPECT_LE(netted.discounted_ee[i], gross + 1e-12);
            netted_area += netted.discounted_ee[i];
            gross_area += gross;
        }
        // A payer against a receiver: netting is worth a lot here.
        EXPECT_LT(netted_area, 0.8 * gross_area);
        EXPECT_NEAR(netted.value_today, a.value_today + b.value_today + c.value_today, 1e-12);
        EXPECT_THROW(exposure_statistics(paths, {0, 0}), InvalidInput);
        EXPECT_THROW(exposure_statistics(paths, {3}), InvalidInput);
        EXPECT_THROW(exposure_statistics(paths, {}, 1.0), InvalidInput);
    }

    TEST(ExposureEngine, ASwapAndItsMirrorNetToNothing)
    {
        const HullWhiteCurveModel m = model();
        const InterestRateSwap swap = par_swap(m, 5.0, true);
        HullWhiteExposureEngine engine(m);
        engine.add(swap, 1.0);
        engine.add(swap, -1.0);
        ExposureSimulationSettings settings;
        settings.paths = 2000;
        const ExposurePaths paths = engine.simulate(settings);
        const ExposureStatistics netted = exposure_statistics(paths);
        const ExposureStatistics alone = exposure_statistics(paths, {0});
        for (std::size_t i = 0; i < netted.times.size(); ++i)
        {
            EXPECT_DOUBLE_EQ(netted.discounted_ee[i], 0.0);
            EXPECT_DOUBLE_EQ(netted.discounted_ene[i], 0.0);
        }
        // The mirror's exposure is the swap's negative exposure.
        const ExposureStatistics mirror = exposure_statistics(paths, {1});
        for (std::size_t i = 0; i < netted.times.size(); ++i)
            EXPECT_DOUBLE_EQ(mirror.discounted_ee[i], -alone.discounted_ene[i]);
    }

    TEST(ExposureEngine, EulerContributionsAddUpToTheNettingSetExposure)
    {
        const HullWhiteCurveModel m = model();
        HullWhiteExposureEngine engine(m);
        engine.add(par_swap(m, 10.0, true));
        engine.add(par_swap(m, 5.0, false));
        engine.add(Swaption{make_swap(2.0, 5.0, 0.04, 1, 4, 100.0, true), 2.0});
        ExposureSimulationSettings settings;
        settings.paths = 5000;
        const ExposurePaths paths = engine.simulate(settings);
        const ExposureStatistics s = exposure_statistics(paths);

        ASSERT_EQ(s.discounted_ee_contributions.size(), 3u);
        bool some_negative = false;
        for (std::size_t i = 0; i < s.times.size(); ++i)
        {
            Real total = 0.0;
            for (const std::vector<Real> &contribution : s.discounted_ee_contributions)
            {
                total += contribution[i];
                some_negative = some_negative || contribution[i] < 0.0;
            }
            EXPECT_NEAR(total, s.discounted_ee[i], 1e-12 * (1.0 + s.discounted_ee[i]));
        }
        // The receiver swap hedges the payer: its marginal exposure is negative.
        EXPECT_TRUE(some_negative);

        // On a subset, the rows follow the order of the requested trades.
        const ExposureStatistics pair = exposure_statistics(paths, {2, 0});
        ASSERT_EQ(pair.discounted_ee_contributions.size(), 2u);
        const std::size_t i = index_of(pair.times, 1.0);
        EXPECT_NEAR(pair.discounted_ee_contributions[0][i] + pair.discounted_ee_contributions[1][i],
                    pair.discounted_ee[i], 1e-12);
    }

    // ── Reproducibility and convergence ──────────────────────────────────────

    TEST(ExposureEngine, SameBitsWhateverTheNumberOfThreads)
    {
        const HullWhiteCurveModel m = model();
        const auto run = [&](ThreadPool *pool)
        {
            HullWhiteExposureEngine engine(m);
            engine.add(par_swap(m, 7.0, true));
            engine.add(Swaption{make_swap(1.0, 4.0, 0.04, 1, 4, 100.0, false), 1.0}, -2.0);
            ExposureSimulationSettings settings;
            settings.paths = 3000; // not a multiple of the chunk size
            settings.keep_cashflows = true;
            return engine.simulate(settings, pool);
        };
        const ExposurePaths serial = run(nullptr);
        ThreadPool one, many;
        one.start(1);
        many.start(8);
        for (ThreadPool *pool : {&one, &many})
        {
            const ExposurePaths parallel = run(pool);
            EXPECT_EQ(parallel.discount_weight, serial.discount_weight);
            EXPECT_EQ(parallel.trade_values, serial.trade_values);
            EXPECT_EQ(parallel.trade_cashflows, serial.trade_cashflows);
            const ExposureStatistics a = exposure_statistics(serial), b = exposure_statistics(parallel);
            EXPECT_EQ(a.discounted_ee, b.discounted_ee);
            EXPECT_EQ(a.pfe, b.pfe);
            EXPECT_EQ(a.eepe, b.eepe);
        }
    }

    TEST(ExposureEngine, TheSeedSelectsThePaths)
    {
        const HullWhiteCurveModel m = model();
        const auto run = [&](std::uint64_t seed)
        {
            HullWhiteExposureEngine engine(m);
            engine.add(par_swap(m, 5.0, true));
            ExposureSimulationSettings settings;
            settings.paths = 500;
            settings.seed = seed;
            return engine.simulate(settings);
        };
        EXPECT_EQ(run(7).trade_values, run(7).trade_values);
        EXPECT_NE(run(7).trade_values, run(8).trade_values);
    }

    TEST(ExposureEngine, StandardErrorDecaysAsOneOverSqrtN)
    {
        // The slope of log(error) against log(N) is measured, not assumed.
        const HullWhiteCurveModel m = model();
        std::vector<Real> log_n, log_error;
        for (const std::size_t paths : {2000u, 8000u, 32000u})
        {
            HullWhiteExposureEngine engine(m);
            engine.add(par_swap(m, 10.0, true));
            ExposureSimulationSettings settings;
            settings.paths = paths;
            const ExposureStatistics s = exposure_statistics(engine.simulate(settings));
            log_n.push_back(std::log(static_cast<Real>(paths)));
            log_error.push_back(std::log(s.discounted_ee_error[index_of(s.times, 3.0)]));
        }
        const Real slope = (log_error.back() - log_error.front()) / (log_n.back() - log_n.front());
        EXPECT_NEAR(slope, -0.5, 0.03);
    }

    TEST(ExposureEngine, RefusesACubeLargerThanTheMemoryLimit)
    {
        const HullWhiteCurveModel m = model();
        HullWhiteExposureEngine engine(m);
        engine.add(par_swap(m, 10.0, true));
        ExposureSimulationSettings settings;
        settings.paths = 100000;
        settings.memory_limit_bytes = std::size_t{1} << 20; // 1 MiB
        EXPECT_THROW(engine.simulate(settings), InvalidInput);
        settings.paths = 0;
        EXPECT_THROW(engine.simulate(settings), InvalidInput);
    }

    // ── X0 meets X1 ──────────────────────────────────────────────────────────

    TEST(ExposureEngine, CvaOfAFiveYearSwapIsCloseToSpreadTimesEpe)
    {
        // The worked example of blueprint §7.6: five-year payer swap,
        // notional 100, counterparty at 200 bp with 40 % recovery. Gregory's
        // rule of thumb CVA ≈ -spread × EPE × T holds within ten percent.
        const HullWhiteCurveModel m = model();
        const Time T = 5.0;
        HullWhiteExposureEngine engine(m);
        engine.add(par_swap(m, T, true));
        ExposureSimulationSettings settings;
        settings.paths = 20000;
        const ExposureStatistics s = exposure_statistics(engine.simulate(settings));

        const Real spread = 0.02, lgd = 0.6;
        const Real cva = cva_unilateral(s.profile(), CreditCurve(hazard_from_spread(spread, lgd)), lgd);
        const Real discounted_epe = expected_positive_exposure(s.times, s.discounted_ee);
        EXPECT_LT(cva, 0.0);
        EXPECT_NEAR(cva / cva_spread_approximation(spread, discounted_epe, T), 1.0, 0.10);
        // Order of magnitude: a few basis points of notional per year.
        EXPECT_GT(-cva, 0.01);
        EXPECT_LT(-cva, 1.0);
    }

} // namespace quantModeling
