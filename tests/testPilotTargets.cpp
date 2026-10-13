#include <gtest/gtest.h>

#include "quantModeling/engines/analytic/swap.hpp"
#include "quantModeling/engines/xva/exposure_engine.hpp"
#include "quantModeling/engines/xva/hull_white_future_value.hpp"
#include "quantModeling/engines/xva/pilot_targets.hpp"
#include "quantModeling/engines/xva/regression_future_value.hpp"
#include "quantModeling/market/curve_bootstrap.hpp"
#include "quantModeling/market/multi_curve_bootstrap.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

// Lot M1 of blueprint/wp/25-ml.md: the pathwise differentials of the pilot
// paths. Z_i = ∂Y_i/∂state_i is checked path by path against a bump of the
// state that is carried along the path, and in conditional mean against the
// derivative of the closed-form value, which it is an unbiased sample of.

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

        /// A swap struck at par, annual fixed against quarterly floating.
        InterestRateSwap par_swap(const HullWhiteCurveModel &m, Time start, Time tenor, bool payer)
        {
            const Real par =
                value_swap(make_swap(start, tenor, 0.0), MultiCurve{m.discount(), m.projection()})
                    .par_rate;
            return make_swap(start, tenor, par, 1, 4, 100.0, payer);
        }

        /// One trade in an engine, bound to the pilot's grid, with the pilot.
        /// The pilot starts from a dispersed state, as it does under the
        /// historical measure.
        struct Bound
        {
            HullWhiteCurveModel m = model();
            HullWhiteExposureEngine engine{m};
            const FutureValue *trade = nullptr;
            PilotPaths pilot;

            Bound(std::unique_ptr<FutureValue> (*make)(const HullWhiteCurveModel &), std::size_t paths)
            {
                std::unique_ptr<FutureValue> owned = make(m);
                trade = owned.get();
                engine.add(std::move(owned));
                ExposureSimulationSettings s;
                s.seed = 2026;
                s.pilot_paths = paths;
                s.pilot_dispersion = 0.01;
                pilot = engine.simulate_pilot(s);
            }
        };

        std::unique_ptr<FutureValue> swap_value(const HullWhiteCurveModel &m)
        {
            return make_hull_white_future_value(par_swap(m, 0.0, 5.0, true), m);
        }

        std::unique_ptr<FutureValue> forward_swap_value(const HullWhiteCurveModel &m)
        {
            return make_hull_white_future_value(par_swap(m, 1.0, 4.0, false), m);
        }

        std::unique_ptr<FutureValue> swaption_value(const HullWhiteCurveModel &m)
        {
            return make_hull_white_future_value(Swaption(par_swap(m, 2.0, 5.0, true), 2.0), m);
        }

        /// The path p with its state at date i moved by `shift`, and every
        /// later state and deflator moved as the dynamics carry it.
        struct MovedPath
        {
            std::vector<Real> state, deflator;

            MovedPath(const PilotPaths &pilot, std::size_t p, std::size_t i, Real shift)
                : state(pilot.state_row(p), pilot.state_row(p) + pilot.dates()),
                  deflator(pilot.deflator_row(p), pilot.deflator_row(p) + pilot.dates())
            {
                for (std::size_t k = i; k < pilot.dates(); ++k)
                {
                    state[k] += shift;
                    deflator[k] *= std::exp(pilot.deflator_slope[k] * shift);
                    if (k + 1 < pilot.dates())
                        shift *= pilot.state_decay[k + 1];
                }
            }

            /// Y_i, summed directly.
            Real target(const FutureValue &trade, std::size_t i) const
            {
                Real future = 0.0;
                for (std::size_t j = i + 1; j < state.size(); ++j)
                    future += trade.cashflow(j, state.data()) * deflator[j];
                return future / deflator[i];
            }

            bool same_regimes(const FutureValue &trade, const PilotPaths &pilot, std::size_t p) const
            {
                for (std::size_t j = 0; j < state.size(); ++j)
                    if (trade.regime(j, state.data()) != trade.regime(j, pilot.state_row(p)))
                        return false;
                return true;
            }
        };

        /// Largest gap between the differentials and central differences of
        /// the targets, over `paths` paths and every date, relative to the
        /// largest differential; `compared` counts the pairs.
        Real worst_gap_to_bumps(const FutureValue &trade, const PilotPaths &pilot,
                                const PilotTargets &targets, std::size_t paths, std::size_t &compared)
        {
            const Real h = 1e-5;
            Real worst = 0.0, scale = 0.0;
            compared = 0;
            for (std::size_t p = 0; p < paths; ++p)
                for (std::size_t i = 0; i < pilot.dates(); ++i)
                {
                    const MovedPath up(pilot, p, i, h), down(pilot, p, i, -h);
                    // A bump that changes an exercise decision is not a
                    // derivative of anything.
                    if (!up.same_regimes(trade, pilot, p) || !down.same_regimes(trade, pilot, p))
                        continue;
                    const Real bumped = (up.target(trade, i) - down.target(trade, i)) / (2.0 * h);
                    const Real z = targets.differential[p * pilot.dates() + i];
                    worst = std::max(worst, std::abs(z - bumped));
                    scale = std::max(scale, std::abs(bumped));
                    ++compared;
                }
            return worst / scale;
        }

        /// ∂value(i, ·)/∂state_i of the closed form, the earlier states held.
        Real exact_delta(const FutureValue &trade, const PilotPaths &pilot, std::size_t p, std::size_t i)
        {
            const Real h = 1e-5;
            std::vector<Real> row(pilot.state_row(p), pilot.state_row(p) + pilot.dates());
            row[i] += h;
            const Real up = trade.value(i, row.data());
            row[i] -= 2.0 * h;
            return (up - trade.value(i, row.data())) / (2.0 * h);
        }

        /**
         * E[Z_i | state_i] = ∂V/∂state_i, tested by its moments: for
         * φ = 1, u and u² (u the standardised state), the mean of
         * (Z_i - ∂V/∂state_i) φ(state_i) is zero. Returns the largest of
         * these means in standard errors, over one date in `stride`.
         */
        Real worst_moment_gap(const FutureValue &trade, const PilotPaths &pilot,
                              const PilotTargets &targets, std::size_t stride)
        {
            const std::size_t n = pilot.dates(), N = pilot.paths;
            Real worst = 0.0;
            for (std::size_t i = 0; i + 1 < n; i += stride)
            {
                Real mean = 0.0, var = 0.0;
                for (std::size_t p = 0; p < N; ++p)
                    mean += pilot.state_row(p)[i];
                mean /= static_cast<Real>(N);
                for (std::size_t p = 0; p < N; ++p)
                    var += (pilot.state_row(p)[i] - mean) * (pilot.state_row(p)[i] - mean);
                const Real sd = std::sqrt(var / static_cast<Real>(N));
                Real sum[3] = {0.0, 0.0, 0.0}, squares[3] = {0.0, 0.0, 0.0};
                for (std::size_t p = 0; p < N; ++p)
                {
                    const Real u = (pilot.state_row(p)[i] - mean) / sd;
                    const Real gap = targets.differential[p * n + i] - exact_delta(trade, pilot, p, i);
                    const Real phi[3] = {1.0, u, u * u};
                    for (int k = 0; k < 3; ++k)
                    {
                        sum[k] += gap * phi[k];
                        squares[k] += gap * phi[k] * gap * phi[k];
                    }
                }
                for (int k = 0; k < 3; ++k)
                {
                    const Real m = sum[k] / static_cast<Real>(N);
                    const Real error =
                        std::sqrt((squares[k] / static_cast<Real>(N) - m * m) / static_cast<Real>(N));
                    if (error > 0.0)
                        worst = std::max(worst, std::abs(m) / error);
                }
            }
            return worst;
        }
    } // namespace

    // ── Path by path ─────────────────────────────────────────────────────────

    TEST(PilotTargets, TheValuesAreTheFutureCashFlowsOverTheDeflator)
    {
        const Bound s(swap_value, 200);
        const PilotTargets targets = pilot_targets(*s.trade, s.pilot);
        ASSERT_EQ(targets.value.size(), s.pilot.paths * s.pilot.dates());
        for (std::size_t p = 0; p < s.pilot.paths; ++p)
        {
            const MovedPath path(s.pilot, p, 0, 0.0);
            for (std::size_t i = 0; i < s.pilot.dates(); ++i)
                EXPECT_NEAR(targets.value[p * s.pilot.dates() + i], path.target(*s.trade, i), 1e-10);
        }
        // Asking for the differentials does not change the values.
        EXPECT_EQ(pilot_targets(*s.trade, s.pilot, false).value, targets.value);
        EXPECT_TRUE(pilot_targets(*s.trade, s.pilot, false).differential.empty());
    }

    TEST(PilotTargets, ASwapsDifferentialsAreTheBumpsOfItsTargets)
    {
        for (const auto make : {swap_value, forward_swap_value})
        {
            const Bound s(make, 200);
            const PilotTargets targets = pilot_targets(*s.trade, s.pilot);
            std::size_t compared = 0;
            EXPECT_LT(worst_gap_to_bumps(*s.trade, s.pilot, targets, 200, compared), 1e-7);
            EXPECT_EQ(compared, 200 * s.pilot.dates()); // a swap has one regime
        }
    }

    TEST(PilotTargets, ASwaptionsDifferentialsAreTheBumpsOfItsTargets)
    {
        const Bound s(swaption_value, 300);
        const PilotTargets targets = pilot_targets(*s.trade, s.pilot);
        std::size_t compared = 0;
        EXPECT_LT(worst_gap_to_bumps(*s.trade, s.pilot, targets, 300, compared), 1e-7);
        // Nearly every pair: a bump of 1e-5 seldom changes the exercise.
        EXPECT_GT(compared, 300 * s.pilot.dates() * 99 / 100);
    }

    TEST(PilotTargets, ABermudansDifferentialsAreTheBumpsOfItsTargets)
    {
        HullWhiteCurveModel m = model();
        std::vector<Time> exercise;
        for (int y = 1; y <= 4; ++y)
            exercise.push_back(static_cast<Time>(y));
        std::unique_ptr<FutureValue> owned =
            make_future_value(BermudanSwaption{par_swap(m, 1.0, 4.0, true), exercise}, m, AmcSettings{});
        FutureValue *trade = owned.get();
        HullWhiteExposureEngine engine(m);
        engine.add(std::move(owned));
        ExposureSimulationSettings settings;
        settings.pilot_paths = 5000;
        settings.pilot_dispersion = 0.01;
        const PilotPaths pilot = engine.simulate_pilot(settings);
        trade->fit(pilot); // the exercise rule its cash flows follow
        const PilotTargets targets = pilot_targets(*trade, pilot);
        std::size_t compared = 0;
        EXPECT_LT(worst_gap_to_bumps(*trade, pilot, targets, 200, compared), 1e-7);
        EXPECT_GT(compared, 200 * pilot.dates() * 95 / 100);
    }

    // ── In conditional mean ──────────────────────────────────────────────────

    TEST(PilotTargets, ASwapsDifferentialsAverageToItsExactDelta)
    {
        const Bound s(swap_value, 40000);
        const PilotTargets targets = pilot_targets(*s.trade, s.pilot);
        EXPECT_LT(worst_moment_gap(*s.trade, s.pilot, targets, 7), 4.0);
    }

    TEST(PilotTargets, ASwaptionsDifferentialsAverageToItsExactDelta)
    {
        // Before the expiry the exact delta is that of the closed-form
        // option; the pathwise differential holds the exercise fixed, and
        // the term it leaves out has no expectation: on the boundary the
        // swap entered is worth nothing.
        const Bound s(swaption_value, 40000);
        const PilotTargets targets = pilot_targets(*s.trade, s.pilot);
        EXPECT_LT(worst_moment_gap(*s.trade, s.pilot, targets, 7), 4.0);
    }

    // ── What they need, and what they cost ───────────────────────────────────

    TEST(PilotTargets, RefusesAPilotWithoutSensitivitiesOrATradeWithoutGradient)
    {
        Bound s(swap_value, 50);
        PilotPaths bare = s.pilot;
        bare.state_decay.clear();
        EXPECT_THROW(pilot_targets(*s.trade, bare), InvalidInput);
        EXPECT_NO_THROW(pilot_targets(*s.trade, bare, false));

        // The default of FutureValue: no gradient.
        struct NoGradient final : FutureValue
        {
            std::vector<Time> event_times() const override { return {1.0}; }
            Time maturity() const override { return 1.0; }
            Real value_today() const override { return 0.0; }
            void bind(const std::vector<Time> &) override {}
            Real value(std::size_t, const Real *) const override { return 0.0; }
            Real cashflow(std::size_t, const Real *state) const override { return state[0]; }
        };
        EXPECT_THROW(pilot_targets(NoGradient{}, s.pilot), InvalidInput);
        EXPECT_NO_THROW(pilot_targets(NoGradient{}, s.pilot, false));
    }

    TEST(PilotTargets, TheSameBitsAsThePilotOfTheSimulation)
    {
        const Bound a(swap_value, 300), b(swap_value, 300);
        EXPECT_EQ(a.pilot.state, b.pilot.state);
        EXPECT_EQ(a.pilot.deflator, b.pilot.deflator);
        EXPECT_TRUE(a.pilot.has_sensitivities());
        EXPECT_EQ(pilot_targets(*a.trade, a.pilot).differential,
                  pilot_targets(*b.trade, b.pilot).differential);
    }

    TEST(PilotTargets, TheDifferentialsCostLessThanFourPassesWithout)
    {
        const Bound s(swap_value, 20000);
        const auto seconds = [&](bool differentials)
        {
            Real best = 1e30;
            for (int repeat = 0; repeat < 5; ++repeat)
            {
                const auto start = std::chrono::steady_clock::now();
                const PilotTargets targets = pilot_targets(*s.trade, s.pilot, differentials);
                best = std::min(best, std::chrono::duration<Real>(
                                          std::chrono::steady_clock::now() - start)
                                          .count());
                EXPECT_FALSE(targets.value.empty());
            }
            return best;
        };
        const Real without = seconds(false), with = seconds(true);
        RecordProperty("seconds_without", std::to_string(without));
        RecordProperty("seconds_with", std::to_string(with));
        EXPECT_LT(with, 4.0 * without) << "without=" << without << " with=" << with;
    }

} // namespace quantModeling
