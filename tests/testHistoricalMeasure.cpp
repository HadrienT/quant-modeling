#include <gtest/gtest.h>

#include "quantModeling/engines/analytic/swap.hpp"
#include "quantModeling/engines/xva/exposure_engine.hpp"
#include "quantModeling/engines/xva/hull_white_future_value.hpp"
#include "quantModeling/market/csa.hpp"
#include "quantModeling/market/historical_rate_dynamics.hpp"
#include "quantModeling/risk/collateral.hpp"
#include "quantModeling/risk/exposure_paths.hpp"
#include "quantModeling/utils/philox.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

// Lot X1b of blueprint/wp/23-xva.md (§3.1): scenarios under the historical
// measure for the risk measures, priced with the risk-neutral model.

namespace quantModeling
{
    namespace
    {
        constexpr Time kDay = 1.0 / 252.0;

        /// An exactly simulated Ornstein-Uhlenbeck path, daily.
        std::vector<Real> ou_path(const HistoricalRateDynamics &d, Real start, std::size_t n, std::uint64_t seed)
        {
            PhiloxGaussianSource gaussian(seed);
            const Real b = std::exp(-d.mean_reversion * kDay);
            const Real sd = d.sigma * std::sqrt((1.0 - b * b) / (2.0 * d.mean_reversion));
            std::vector<Real> path = {start};
            for (std::size_t k = 1; k < n; ++k)
                path.push_back(d.long_run_rate + (path.back() - d.long_run_rate) * b + sd * gaussian.next());
            return path;
        }

        /// Flat 3 % curve, risk-neutral Hull-White a = 3 %, σ = 1 %.
        HullWhiteCurveModel model()
        {
            return HullWhiteCurveModel(0.03, 0.01, DiscountCurve(0.03));
        }

        InterestRateSwap par_payer(const HullWhiteCurveModel &m, Time tenor = 10.0)
        {
            const MultiCurve curves{m.discount(), m.projection()};
            const Real par = value_swap(make_swap(0.0, tenor, 0.0), curves).par_rate;
            return make_swap(0.0, tenor, par, 1, 4, 100.0, true);
        }

        ExposurePaths simulate(const HullWhiteCurveModel &m, const std::optional<HistoricalRateDynamics> &historical,
                               std::size_t paths = 10000, ThreadPool *pool = nullptr)
        {
            HullWhiteExposureEngine engine(m);
            engine.add(par_payer(m));
            ExposureSimulationSettings settings;
            settings.paths = paths;
            settings.historical = historical;
            return engine.simulate(settings, pool);
        }

        std::size_t index_of(const std::vector<Time> &grid, Time t)
        {
            for (std::size_t i = 0; i < grid.size(); ++i)
                if (std::abs(grid[i] - t) < 1e-9)
                    return i;
            ADD_FAILURE() << "date " << t << " is not on the grid";
            return 0;
        }

        /// Standard deviation of the swap's value at one date.
        Real value_sd(const ExposurePaths &paths, std::size_t i)
        {
            const std::size_t n = paths.dates();
            Real sum = 0.0, sum2 = 0.0;
            for (std::size_t p = 0; p < paths.paths; ++p)
            {
                const Real v = paths.trade_values[0][p * n + i];
                sum += v;
                sum2 += v * v;
            }
            const Real mean = sum / static_cast<Real>(paths.paths);
            return std::sqrt(sum2 / static_cast<Real>(paths.paths) - mean * mean);
        }
    } // namespace

    // ── Estimating the dynamics ──────────────────────────────────────────────

    TEST(HistoricalRateDynamics, EstimatorRecoversTheParametersThatGeneratedThePath)
    {
        // Forty years of daily data: the volatility is pinned down, the mean
        // reversion and the level only to their (reported) standard errors.
        const HistoricalRateDynamics truth{0.25, 0.035, 0.012};
        const HistoricalRateEstimate e = estimate_historical_rate_dynamics(ou_path(truth, 0.02, 10080, 11), kDay);
        EXPECT_EQ(e.observations, 10080u);
        EXPECT_NEAR(e.dynamics.sigma, truth.sigma, 4.0 * e.sigma_std_error);
        EXPECT_NEAR(e.dynamics.sigma / truth.sigma, 1.0, 0.03);
        EXPECT_NEAR(e.dynamics.mean_reversion, truth.mean_reversion, 4.0 * e.mean_reversion_std_error);
        EXPECT_NEAR(e.dynamics.long_run_rate, truth.long_run_rate, 4.0 * e.long_run_rate_std_error);
        // What the standard errors say: σ to about 1 %, a not even to 30 %.
        EXPECT_LT(e.sigma_std_error / e.dynamics.sigma, 0.01);
        EXPECT_GT(e.mean_reversion_std_error / e.dynamics.mean_reversion, 0.2);
        EXPECT_NEAR(e.dynamics.half_life(), std::log(2.0) / e.dynamics.mean_reversion, 1e-15);
    }

    TEST(HistoricalRateDynamics, EstimatesAreUnbiasedAcrossSamplesForTheVolatility)
    {
        // The average over independent samples sits on the true σ.
        const HistoricalRateDynamics truth{0.5, 0.03, 0.009};
        Real sum = 0.0;
        const int samples = 40;
        for (int s = 0; s < samples; ++s)
            sum += estimate_historical_rate_dynamics(ou_path(truth, 0.03, 2520, 100 + static_cast<std::uint64_t>(s)), kDay)
                       .dynamics.sigma;
        EXPECT_NEAR(sum / samples / truth.sigma, 1.0, 0.01);
    }

    TEST(HistoricalRateDynamics, VolatilityAloneNeedsNoMeanReversion)
    {
        const HistoricalRateDynamics truth{0.25, 0.035, 0.012};
        const std::vector<Real> path = ou_path(truth, 0.02, 5040, 3);
        EXPECT_NEAR(estimate_historical_volatility(path, kDay) / truth.sigma, 1.0, 0.03);
        // The two estimators agree: over a day the drift is negligible.
        EXPECT_NEAR(estimate_historical_volatility(path, kDay) /
                        estimate_historical_rate_dynamics(path, kDay).dynamics.sigma,
                    1.0, 0.002);
    }

    TEST(HistoricalRateDynamics, RefusesWhatItCannotEstimate)
    {
        EXPECT_THROW(estimate_historical_rate_dynamics(std::vector<Real>(10, 0.03), kDay), InvalidInput);  // too short
        EXPECT_THROW(estimate_historical_rate_dynamics(std::vector<Real>(100, 0.03), kDay), InvalidInput); // constant
        // A steady trend has no level to revert to.
        std::vector<Real> trend;
        for (int k = 0; k < 500; ++k)
            trend.push_back(0.01 + 1e-5 * k + 1e-6 * std::sin(k));
        EXPECT_THROW(estimate_historical_rate_dynamics(trend, kDay), InvalidInput);
        EXPECT_NO_THROW(estimate_historical_volatility(trend, kDay));
        EXPECT_THROW(estimate_historical_rate_dynamics(ou_path({0.25, 0.03, 0.01}, 0.03, 100, 1), 0.0), InvalidInput);
        HistoricalRateDynamics bad{0.0, 0.03, 0.01};
        EXPECT_THROW(bad.validate(), InvalidInput);
    }

    // ── Scenarios under the historical measure ───────────────────────────────

    TEST(HistoricalMeasure, CubeIsFlaggedUndiscountedAndClosedToXva)
    {
        const HullWhiteCurveModel m = model();
        const ExposurePaths historical = simulate(m, HistoricalRateDynamics{0.2, 0.03, 0.008}, 500);
        EXPECT_EQ(historical.measure, ExposureMeasure::Historical);
        EXPECT_TRUE(std::all_of(historical.discount_weight.begin(), historical.discount_weight.end(),
                                [](Real w)
                                { return w == 1.0; }));
        EXPECT_TRUE(std::all_of(historical.discount.begin(), historical.discount.end(), [](Real d)
                                { return d == 1.0; }));

        const ExposureStatistics s = exposure_statistics(historical);
        EXPECT_EQ(s.measure, ExposureMeasure::Historical);
        EXPECT_EQ(s.ee, s.discounted_ee); // one family of profiles
        // CVA is a price: it must not be computed on real-world scenarios.
        EXPECT_THROW(s.profile(), InvalidInput);
        EXPECT_NO_THROW(exposure_statistics(simulate(m, std::nullopt, 500)).profile());

        // Collateral keeps the flag.
        HullWhiteExposureEngine engine(m);
        engine.add(par_payer(m));
        ExposureSimulationSettings settings;
        settings.paths = 200;
        settings.historical = HistoricalRateDynamics{0.2, 0.03, 0.008};
        settings.grid.margin_period_of_risk = 10.0 / 250.0;
        Csa csa;
        EXPECT_EQ(collateralise(engine.simulate(settings), csa).measure, ExposureMeasure::Historical);
    }

    TEST(HistoricalMeasure, ARatePinnedOnTheForwardCurveGivesOneDeterministicScenario)
    {
        // Flat 3 % curve, rate pinned at 3 % (strong reversion, almost no
        // noise): the state x = r - f(0, t) stays at zero on every path, and
        // the swap is worth what the pricing model says at x = 0 — small and
        // positive for a payer: the model's bond prices at x = 0 sit a
        // convexity term below the forward prices.
        const HullWhiteCurveModel m = model();
        const InterestRateSwap swap = par_payer(m);
        const ExposurePaths paths = simulate(m, HistoricalRateDynamics{5.0, 0.03, 1e-7}, 200);
        const ExposureStatistics s = exposure_statistics(paths);

        const std::unique_ptr<FutureValue> value = make_future_value(swap, m);
        value->bind(paths.times);
        const std::vector<Real> at_zero(paths.dates(), 0.0);
        for (std::size_t i = 0; i < s.times.size(); ++i)
        {
            const Real expected = value->value(i, at_zero.data());
            EXPECT_NEAR(s.efv[i], expected, 2e-4) << "t=" << s.times[i];
            EXPECT_NEAR(s.pfe[i], std::max(expected, 0.0), 2e-4) << "t=" << s.times[i];
            EXPECT_LT(value_sd(paths, i), 1e-4);
        }
        // A payer swap at par, three years on, with the forwards realised:
        // well under one percent of the notional.
        EXPECT_LT(std::abs(s.efv[index_of(s.times, 3.0)]), 1.0);
    }

    TEST(HistoricalMeasure, TheLongRunLevelDrivesTheExpectedValue)
    {
        // The payer swap gains when rates end up above the forwards: its
        // expected future value follows the level history says rates revert to.
        const HullWhiteCurveModel m = model();
        const auto efv_at_3y = [&](Real long_run_rate)
        {
            const ExposureStatistics s =
                exposure_statistics(simulate(m, HistoricalRateDynamics{0.5, long_run_rate, 0.008}, 5000));
            return s.efv[index_of(s.times, 3.0)];
        };
        const Real low = efv_at_3y(0.02), same = efv_at_3y(0.03), high = efv_at_3y(0.04);
        EXPECT_LT(low, -1.0);
        EXPECT_GT(high, 1.0);
        EXPECT_LT(low, same);
        EXPECT_LT(same, high);
        EXPECT_NEAR(same, 0.0, 0.5); // reverting to the forward level: no drift in value
    }

    TEST(HistoricalMeasure, ExposureScalesWithTheHistoricalVolatility)
    {
        // The value of a swap is close to linear in the rate: twice the
        // volatility, twice the dispersion, twice the PFE of an at-market swap.
        const HullWhiteCurveModel m = model();
        const ExposurePaths calm = simulate(m, HistoricalRateDynamics{0.2, 0.03, 0.006}, 20000);
        const ExposurePaths agitated = simulate(m, HistoricalRateDynamics{0.2, 0.03, 0.012}, 20000);
        const std::size_t i = index_of(calm.times, 3.0);
        EXPECT_NEAR(value_sd(agitated, i) / value_sd(calm, i), 2.0, 0.05);
        const ExposureStatistics a = exposure_statistics(calm), b = exposure_statistics(agitated);
        EXPECT_NEAR(b.pfe[i] / a.pfe[i], 2.0, 0.15);
        // The expected exposure less than doubles: the value has a small
        // positive mean (the convexity term of the pricing model) that does
        // not scale with the historical volatility.
        EXPECT_GT(b.ee[i] / a.ee[i], 1.7);
        EXPECT_LT(b.ee[i] / a.ee[i], 2.0);
    }

    TEST(HistoricalMeasure, DispersionOfTheRateIsTheOrnsteinUhlenbeckOne)
    {
        // sd of the swap value at t ≈ |dV/dr| × sd of r(t), and
        // sd r(t) = σ sqrt((1 - e^{-2at}) / (2a)): compare two dates, where
        // the sensitivity |dV/dr| is read off a small bump of the same engine.
        const HullWhiteCurveModel m = model();
        const HistoricalRateDynamics d{0.3, 0.03, 0.009};
        const ExposurePaths paths = simulate(m, d, 40000);
        const auto rate_sd = [&](Time t)
        {
            return d.sigma * std::sqrt((1.0 - std::exp(-2.0 * d.mean_reversion * t)) / (2.0 * d.mean_reversion));
        };
        // Tiny volatility around a shifted level: V(t) is then a deterministic
        // function of r(t) = θ, and its slope in θ is dV/dr.
        const auto value_at_level = [&](Real level, Time t)
        {
            const ExposureStatistics s = exposure_statistics(simulate(m, HistoricalRateDynamics{50.0, level, 1e-7}, 4));
            return s.efv[index_of(s.times, t)];
        };
        for (const Time t : {2.0, 5.0})
        {
            const Real slope = (value_at_level(0.031, t) - value_at_level(0.029, t)) / 0.002;
            EXPECT_NEAR(value_sd(paths, index_of(paths.times, t)) / (std::abs(slope) * rate_sd(t)), 1.0, 0.04)
                << "t=" << t;
        }
    }

    TEST(HistoricalMeasure, DiffersFromTheRiskNeutralOneOnlyThroughTheScenarios)
    {
        // Same trade, same pricing model, same seed: a lower historical
        // volatility than the implied one gives a lower PFE.
        const HullWhiteCurveModel m = model(); // implied σ = 1 %
        const ExposureStatistics neutral = exposure_statistics(simulate(m, std::nullopt, 20000));
        const ExposureStatistics historical =
            exposure_statistics(simulate(m, HistoricalRateDynamics{0.03, 0.03, 0.007}, 20000));
        const std::size_t i = index_of(neutral.times, 3.0);
        EXPECT_NEAR(historical.pfe[i] / neutral.pfe[i], 0.7, 0.08);
        // Today's value is a price: the same under both.
        EXPECT_DOUBLE_EQ(historical.value_today, neutral.value_today);
    }

    TEST(HistoricalMeasure, SameBitsWhateverTheNumberOfThreads)
    {
        const HullWhiteCurveModel m = model();
        const HistoricalRateDynamics d{0.2, 0.035, 0.008};
        const ExposurePaths serial = simulate(m, d, 3000);
        ThreadPool pool;
        pool.start(8);
        const ExposurePaths parallel = simulate(m, d, 3000, &pool);
        EXPECT_EQ(parallel.trade_values, serial.trade_values);
        EXPECT_EQ(exposure_statistics(parallel).pfe, exposure_statistics(serial).pfe);
    }

    TEST(HistoricalMeasure, RejectsInvalidDynamics)
    {
        const HullWhiteCurveModel m = model();
        EXPECT_THROW(simulate(m, HistoricalRateDynamics{0.0, 0.03, 0.01}, 10), InvalidInput);
        EXPECT_THROW(simulate(m, HistoricalRateDynamics{0.2, 0.03, 0.0}, 10), InvalidInput);
        // A curve held flat in discount factor before its first pillar jumps
        // at t = 0: there is no short rate to start from.
        const HullWhiteCurveModel jump(0.03, 0.01, DiscountCurve({1.0, 10.0}, {0.97, 0.74}));
        EXPECT_THROW(simulate(jump, HistoricalRateDynamics{0.2, 0.03, 0.008}, 10), InvalidInput);
        const HullWhiteCurveModel smooth(
            0.03, 0.01, DiscountCurve({1.0, 10.0}, {0.97, 0.74}, CurveExtrapolation::FlatForward));
        EXPECT_NO_THROW(simulate(smooth, HistoricalRateDynamics{0.2, 0.03, 0.008}, 10));
    }

} // namespace quantModeling
