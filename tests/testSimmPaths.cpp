#include <gtest/gtest.h>

#include "quantModeling/engines/analytic/hull_white_swaption.hpp"
#include "quantModeling/engines/analytic/swap.hpp"
#include "quantModeling/engines/xva/exposure_engine.hpp"
#include "quantModeling/engines/xva/hull_white_future_value.hpp"
#include "quantModeling/risk/collateral.hpp"
#include "quantModeling/risk/initial_margin.hpp"
#include "quantModeling/risk/simm.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

// Lot X5b of blueprint/wp/23-xva.md: SIMM from sensitivities, today and on
// every path. The sensitivities come out of the closed forms; the oracle is
// a bump of the curve and a full revaluation.

namespace quantModeling
{
    namespace
    {
        constexpr Real kA = 0.03, kSigma = 0.01;

        /// A curve given by its zero rate, on quarterly pillars to 40 years.
        DiscountCurve curve(const std::function<Real(Time)> &zero)
        {
            std::vector<Time> times;
            std::vector<Real> dfs;
            for (int k = 1; k <= 160; ++k)
            {
                const Time t = 0.25 * k;
                times.push_back(t);
                dfs.push_back(std::exp(-zero(t) * t));
            }
            return DiscountCurve(times, dfs, CurveExtrapolation::FlatForward);
        }

        Real base_zero(Time t)
        {
            return 0.035 + 0.004 * (1.0 - std::exp(-t / 5.0));
        }

        /// +1 bp on the zero rate at SIMM vertex k, tapering linearly to the
        /// neighbouring vertices (flat beyond the first and the last).
        Real bump(std::size_t k, Time t)
        {
            const auto &v = simm::vertex_years;
            if (k == 0 && t <= v[0])
                return 1e-4;
            if (k == simm::vertices - 1 && t >= v[k])
                return 1e-4;
            if (k > 0 && t > v[k - 1] && t <= v[k])
                return 1e-4 * (t - v[k - 1]) / (v[k] - v[k - 1]);
            if (k + 1 < simm::vertices && t > v[k] && t < v[k + 1])
                return 1e-4 * (v[k + 1] - t) / (v[k + 1] - v[k]);
            return 0.0;
        }

        HullWhiteCurveModel model(const std::function<Real(Time)> &zero = base_zero)
        {
            return HullWhiteCurveModel(kA, kSigma, curve(zero));
        }

        InterestRateSwap payer(const HullWhiteCurveModel &m, Time start, Time tenor, Real notional = 1e7)
        {
            const Real par = value_swap(make_swap(start, tenor, 0.0, 1, 1),
                                        MultiCurve{m.discount(), m.projection()})
                                 .par_rate;
            return make_swap(start, tenor, par, 1, 1, notional, true);
        }

        /// PV01 by vertex from what a trade reports about itself.
        simm::Sensitivities reported(const FutureValue &trade)
        {
            std::vector<BondExposure> bonds;
            std::vector<VolExposure> vols;
            EXPECT_TRUE(trade.sensitivities_today(bonds, vols));
            simm::Sensitivities s;
            for (const BondExposure &b : bonds)
                if (b.maturity > 0.0)
                    s.add_delta(b.maturity, -b.maturity * b.amount * 1e-4);
            for (const VolExposure &v : vols)
                s.add_vega(v.expiry, v.vega_times_vol);
            return s;
        }

        /// PV01 by vertex by bumping the curve and revaluing.
        template <typename Instrument>
        std::array<Real, simm::vertices> bumped(const Instrument &instrument)
        {
            const HullWhiteCurveModel m = model();
            const Real base = make_hull_white_future_value(instrument, m)->value_today();
            std::array<Real, simm::vertices> pv01{};
            for (std::size_t k = 0; k < simm::vertices; ++k)
            {
                const HullWhiteCurveModel up = model([k](Time t)
                                                     { return base_zero(t) + bump(k, t); });
                pv01[k] = make_hull_white_future_value(instrument, up)->value_today() - base;
            }
            return pv01;
        }
    } // namespace

    TEST(SimmPaths, TheReportedDeltasAreThoseOfABumpedCurve)
    {
        const HullWhiteCurveModel m = model();
        // A spot swap, a forward swap, and a swaption out of and in the money.
        const InterestRateSwap spot = payer(m, 0.0, 10.0), forward = payer(m, 3.0, 7.0);
        InterestRateSwap cheap = payer(m, 2.0, 8.0), dear = cheap;
        cheap.fixed_rate += 0.01;
        dear.fixed_rate -= 0.01;
        const auto check = [&](const Instrument &instrument, const std::array<Real, 12> &by_bump)
        {
            const simm::Sensitivities s = reported(*make_hull_white_future_value(instrument, m));
            Real scale = 0.0;
            for (const Real x : by_bump)
                scale = std::max(scale, std::abs(x));
            ASSERT_GT(scale, 0.0);
            for (std::size_t k = 0; k < simm::vertices; ++k)
                EXPECT_NEAR(s.delta[k], by_bump[k], 0.01 * scale) << "vertex " << k;
        };
        check(spot, bumped(spot));
        check(forward, bumped(forward));
        // An option: the exercise boundary moves with the curve, and at first
        // order that changes nothing (the value there is zero).
        for (const InterestRateSwap &underlying : {payer(m, 2.0, 8.0), cheap, dear})
        {
            const Swaption swaption{underlying, 2.0};
            check(swaption, bumped(swaption));
        }
        // A payer swap of 10 M over 10 years: about 8 000 a basis point,
        // lost when rates fall — a positive PV01, mostly at the 10-year vertex.
        const simm::Sensitivities s = reported(*make_hull_white_future_value(spot, m));
        EXPECT_GT(s.delta[8], 5000.0);
        EXPECT_LT(s.delta[8], 9000.0);
    }

    TEST(SimmPaths, TheReportedVegaIsTheSensitivityToTheModelsVolatility)
    {
        // Under Hull-White the normal volatility of a swap rate is
        // proportional to σ: raising σ by 1 % raises every implied vol by
        // 1 %, so the price moves by 1 % of σ_N ∂V/∂σ_N.
        const HullWhiteCurveModel m = model();
        const Swaption swaption{payer(m, 2.0, 8.0), 2.0};
        const simm::Sensitivities s = reported(*make_hull_white_future_value(swaption, m));
        const HullWhiteCurveModel up(kA, kSigma * 1.01, m.discount());
        const Real by_bump = (hull_white_european_swaption(swaption, up) -
                              hull_white_european_swaption(swaption, m)) /
                             0.01;
        Real vega = 0.0;
        for (const Real v : s.vega)
            vega += v;
        EXPECT_GT(vega, 0.0);
        EXPECT_NEAR(vega, by_bump, 0.02 * by_bump);
        // Expiring in two years: on the 2-year vertex.
        EXPECT_NEAR(s.vega[5], vega, 1e-9 * vega);
        // A swap has no vega.
        const simm::Sensitivities linear = reported(*make_hull_white_future_value(swaption.swap, m));
        for (const Real v : linear.vega)
            EXPECT_EQ(v, 0.0);
    }

    TEST(SimmPaths, TodaysSimmIsTheOneComputedDirectlyFromBumpedSensitivities)
    {
        const HullWhiteCurveModel m = model();
        const InterestRateSwap a = payer(m, 0.0, 10.0), b = payer(m, 0.0, 5.0, 6e6);
        HullWhiteExposureEngine engine(m);
        engine.add(a);
        engine.add(b, -1.0);
        ExposureSimulationSettings settings;
        settings.paths = 200;
        settings.simm = true;
        const ExposurePaths paths = engine.simulate(settings);

        simm::Sensitivities direct;
        const std::array<Real, 12> da = bumped(a), db = bumped(b);
        for (std::size_t k = 0; k < simm::vertices; ++k)
            direct.delta[k] = da[k] - db[k];
        const simm::Margin expected = simm::interest_rate_margin(direct);
        EXPECT_NEAR(paths.simm_today.delta, expected.delta, 0.01 * expected.delta);
        EXPECT_EQ(paths.simm_today.vega, 0.0);
        EXPECT_EQ(paths.simm_today.curvature, 0.0);
        // Order of magnitude: 60 bp on a net PV01 of a few thousand a bp.
        EXPECT_GT(paths.simm_today.total(), 100000.0);
        EXPECT_LT(paths.simm_today.total(), 600000.0);
        ASSERT_EQ(paths.simm.size(), paths.paths * paths.dates());
    }

    TEST(SimmPaths, OnAPathTheSimmIsThatOfTheSameTradeRevaluedInThatScenario)
    {
        // At a coupon date t on a path, what is left of the swap is a spot
        // swap on the curve of that scenario, P(t, t + u | x): its SIMM,
        // computed from scratch on that curve, is the path's.
        const HullWhiteCurveModel m = model();
        const InterestRateSwap swap = payer(m, 0.0, 10.0);
        HullWhiteExposureEngine engine(m);
        engine.add(swap);
        ExposureSimulationSettings settings;
        settings.paths = 40;
        settings.simm = true;
        const ExposurePaths paths = engine.simulate(settings);
        const std::size_t n = paths.dates();
        std::size_t i = 0;
        while (paths.times[i] < 4.0 - 1e-9)
            ++i;
        const Time t = paths.times[i];
        for (std::size_t p = 0; p < paths.paths; p += 7)
        {
            // The state of the path, from its discount weight
            // P(0, T*) / P(t, T* | x) — any bond gives it back.
            const Time horizon = paths.times.back();
            const Real target = paths.discount_weight[p * n + i];
            Real lo = -0.5, hi = 0.5;
            for (int it = 0; it < 200; ++it)
            {
                const Real mid = 0.5 * (lo + hi);
                (m.discount().discount(horizon) / m.zcb(t, horizon, mid) < target ? lo : hi) = mid;
            }
            const Real x = 0.5 * (lo + hi);
            const HullWhiteCurveModel scenario(
                kA, kSigma, curve([&](Time u)
                                  { return -std::log(m.zcb(t, t + u, x)) / u; }));
            HullWhiteExposureEngine fresh(scenario);
            fresh.add(make_swap(0.0, 6.0, swap.fixed_rate, 1, 1, swap.notional, true));
            ExposureSimulationSettings one = settings;
            one.paths = 2;
            const Real direct = fresh.simulate(one).simm_today.total();
            EXPECT_NEAR(paths.simm[p * n + i], direct, 0.005 * direct) << "path " << p;
        }
    }

    TEST(SimmPaths, TheMarginRunsOffNetsAndIsTheSameForBothParties)
    {
        const HullWhiteCurveModel m = model();
        const auto run = [&](Real quantity, bool hedge)
        {
            HullWhiteExposureEngine engine(m);
            engine.add(payer(m, 0.0, 10.0), quantity);
            if (hedge)
                engine.add(payer(m, 0.0, 10.0, 8e6), -quantity);
            ExposureSimulationSettings settings;
            settings.paths = 2000;
            settings.simm = true;
            return engine.simulate(settings);
        };
        const ExposurePaths ours = run(1.0, false), theirs = run(-1.0, false), hedged = run(1.0, true);
        const std::size_t n = ours.dates();
        // Both parties compute the same margin from the same sensitivities.
        EXPECT_NEAR(theirs.simm_today.total(), ours.simm_today.total(), 1e-9);
        for (std::size_t c = 0; c < ours.simm.size(); c += 101)
            ASSERT_NEAR(theirs.simm[c], ours.simm[c], 1e-6 * (1.0 + ours.simm[c]));
        // Nothing left to margin at maturity; less and less before.
        const auto mean_at = [&](const ExposurePaths &paths, Time t)
        {
            std::size_t i = 0;
            while (paths.times[i] < t - 1e-9)
                ++i;
            Real sum = 0.0;
            for (std::size_t p = 0; p < paths.paths; ++p)
                sum += paths.simm[p * n + i];
            return sum / static_cast<Real>(paths.paths);
        };
        EXPECT_GT(ours.simm_today.total(), mean_at(ours, 3.0));
        EXPECT_GT(mean_at(ours, 3.0), mean_at(ours, 7.0));
        EXPECT_GT(mean_at(ours, 7.0), mean_at(ours, 9.5));
        EXPECT_EQ(mean_at(ours, 10.0), 0.0);
        for (const Real x : ours.simm)
            ASSERT_GE(x, 0.0);
        // An 80 % hedge leaves a fifth of the margin.
        EXPECT_NEAR(hedged.simm_today.total(), 0.2 * ours.simm_today.total(),
                    1e-6 * ours.simm_today.total());
    }

    TEST(SimmPaths, ASwaptionAddsVegaAndCurvatureUntilItExpires)
    {
        const HullWhiteCurveModel m = model();
        const Swaption swaption{payer(m, 2.0, 8.0), 2.0};
        HullWhiteExposureEngine engine(m);
        engine.add(swaption);
        ExposureSimulationSettings settings;
        settings.paths = 500;
        settings.simm = true;
        const ExposurePaths paths = engine.simulate(settings);
        EXPECT_GT(paths.simm_today.delta, 0.0);
        EXPECT_GT(paths.simm_today.vega, 0.0);
        EXPECT_GT(paths.simm_today.curvature, 0.0);
        // At the money, half the delta of the swap it gives the right to.
        HullWhiteExposureEngine linear(m);
        linear.add(swaption.swap);
        const Real swap_delta = linear.simulate(settings).simm_today.delta;
        EXPECT_GT(paths.simm_today.delta, 0.35 * swap_delta);
        EXPECT_LT(paths.simm_today.delta, 0.65 * swap_delta);
        // After the expiry: the swap's margin on the paths that exercised,
        // nothing on the others.
        const std::size_t n = paths.dates();
        std::size_t after = 0;
        while (paths.times[after] < 3.0 - 1e-9)
            ++after;
        std::size_t exercised = 0, expired = 0;
        for (std::size_t p = 0; p < paths.paths; ++p)
        {
            const bool worth = paths.trade_values[0][p * n + after] != 0.0;
            (worth ? exercised : expired) += 1;
            if (!worth)
                ASSERT_EQ(paths.simm[p * n + after], 0.0);
            else
                ASSERT_GT(paths.simm[p * n + after], 0.0);
        }
        EXPECT_GT(exercised, 100u);
        EXPECT_GT(expired, 100u);
    }

    TEST(SimmPaths, ATradeValuedByRegressionIsRefusedAndTheMarginIsOptional)
    {
        const HullWhiteCurveModel m = model();
        HullWhiteExposureEngine engine(m);
        engine.add(payer(m, 0.0, 5.0));
        ExposureSimulationSettings settings;
        settings.paths = 300;
        EXPECT_TRUE(engine.simulate(settings).simm.empty());
        EXPECT_EQ(engine.simulate(settings).simm_today.total(), 0.0);

        std::vector<Time> exercise = {1.0, 2.0, 3.0};
        engine.add(BermudanSwaption{payer(m, 1.0, 4.0), exercise});
        settings.simm = true;
        EXPECT_THROW(engine.simulate(settings), InvalidInput);

        // The same bits on one thread or several.
        HullWhiteExposureEngine linear(m);
        linear.add(payer(m, 0.0, 5.0));
        linear.add(Swaption{payer(m, 1.0, 4.0), 1.0}, -1.0);
        settings.paths = 1000;
        ThreadPool pool;
        pool.start(6);
        EXPECT_EQ(linear.simulate(settings, &pool).simm, linear.simulate(settings).simm);
    }

    TEST(SimmPaths, SimmAndTheRegressionModelAgreeOnTheShapeOfTheMargin)
    {
        // Two models of the same thing: ten days of rate moves at 99 %. The
        // regression reads it off this model's own volatility (100 bp a
        // year), SIMM off ISDA's risk weights (60 bp over ten days at ten
        // years, about 130 bp a year): the same profile, at a ratio near
        // that of the two volatilities.
        const HullWhiteCurveModel m = model();
        const Time mpor = 10.0 / 250.0;
        HullWhiteExposureEngine engine(m);
        engine.add(payer(m, 0.0, 10.0));
        ExposureSimulationSettings settings;
        settings.paths = 10000;
        settings.simm = true;
        settings.keep_cashflows = true;
        settings.grid.margin_period_of_risk = mpor;
        const ExposurePaths paths = engine.simulate(settings);
        const InitialMargin dim = DynamicInitialMargin::fit(paths, mpor).margin(paths);
        const std::vector<MarginPeriod> periods = margin_periods(paths.times, mpor);
        const std::size_t n = paths.dates();

        const Real ratio_today = paths.simm_today.total() / dim.today;
        EXPECT_GT(ratio_today, 1.0);
        EXPECT_LT(ratio_today, 2.0);
        for (std::size_t r = 0; r < periods.size(); ++r)
        {
            if (periods[r].lagged == MarginPeriod::kToday || dim.times[r] > 8.5)
                continue;
            // The margin in place at the reporting date was set at the
            // lagged one.
            Real simm_mean = 0.0;
            for (std::size_t p = 0; p < paths.paths; ++p)
                simm_mean += paths.simm[p * n + periods[r].lagged];
            simm_mean /= static_cast<Real>(paths.paths);
            EXPECT_GT(simm_mean / dim.expected[r], 0.7 * ratio_today) << dim.times[r];
            EXPECT_LT(simm_mean / dim.expected[r], 1.4 * ratio_today) << dim.times[r];
        }
    }

} // namespace quantModeling
