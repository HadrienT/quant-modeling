#include <gtest/gtest.h>

#include "quantModeling/engines/analytic/swap.hpp"
#include "quantModeling/engines/xva/exposure_engine.hpp"
#include "quantModeling/engines/xva/exposure_program.hpp"
#include "quantModeling/engines/xva/hull_white_future_value.hpp"
#include "quantModeling/gpu/device.hpp"
#include "quantModeling/risk/collateral.hpp"
#include "quantModeling/risk/xva_report.hpp"
#include "quantModeling/utils/philox.hpp"

#include <cmath>
#include <memory>
#include <vector>

// Lot X7 of blueprint/wp/23-xva.md, the part that needs no GPU: a trade
// written as a flat program is the trade, and the exposure of a netting set
// asked for without its cube is the one read off the cube. The device runs
// the same functions (tests/gpu/testGpuExposure.cpp).

namespace quantModeling
{
    namespace
    {
        constexpr Real kA = 0.03, kSigma = 0.01;

        DiscountCurve curve(Real level, Real slope)
        {
            std::vector<Time> times;
            std::vector<Real> dfs;
            for (int k = 1; k <= 160; ++k)
            {
                const Time t = 0.25 * k;
                times.push_back(t);
                dfs.push_back(std::exp(-(level + slope * (1.0 - std::exp(-t / 5.0))) * t));
            }
            return DiscountCurve(times, dfs, CurveExtrapolation::FlatForward);
        }

        /// Two curves: the projection curve sits above the discount curve.
        HullWhiteCurveModel model()
        {
            return HullWhiteCurveModel(kA, kSigma, curve(0.035, 0.004), curve(0.038, 0.005));
        }

        InterestRateSwap swap(const HullWhiteCurveModel &m, Time start, Time tenor, bool payer,
                              Real moneyness = 0.0, Real spread = 0.0)
        {
            const Real par = value_swap(make_swap(start, tenor, 0.0, 1, 2),
                                        MultiCurve{m.discount(), m.projection()})
                                 .par_rate;
            return make_swap(start, tenor, par + moneyness, 1, 2, 1e7, payer, spread);
        }

        /// A book with every kind of term: swaps on both sides (one started,
        /// one forward, one with a spread), swaptions bought and sold.
        void fill(HullWhiteExposureEngine &engine, const HullWhiteCurveModel &m)
        {
            engine.add(swap(m, 0.0, 10.0, true));
            engine.add(swap(m, 1.0, 6.0, false, 0.004, 0.001), 2.0);
            engine.add(Swaption{swap(m, 2.0, 5.0, true, 0.002), 2.0});
            engine.add(Swaption{swap(m, 3.0, 4.0, false, -0.001), 3.0}, -1.5);
        }

        ExposureSimulationSettings settings(std::size_t paths)
        {
            ExposureSimulationSettings s;
            s.paths = paths;
            s.seed = 2026;
            s.keep_cashflows = true;
            return s;
        }
    } // namespace

    TEST(ExposureProgram, ACompiledTradeGivesItsOwnValuesAndCashFlows)
    {
        const HullWhiteCurveModel m = model();
        std::vector<std::unique_ptr<FutureValue>> trades;
        trades.push_back(make_future_value(swap(m, 0.0, 10.0, true), m));
        trades.push_back(make_future_value(swap(m, 1.0, 6.0, false, 0.004, 0.001), m));
        trades.push_back(make_future_value(Swaption{swap(m, 2.0, 5.0, true, 0.002), 2.0}, m));
        trades.push_back(make_future_value(Swaption{swap(m, 3.0, 4.0, false, -0.001), 3.0}, m));
        std::vector<Time> events;
        for (const auto &trade : trades)
            for (const Time t : trade->event_times())
                events.push_back(t);
        const std::vector<Time> grid = exposure_grid(10.0, events);
        xva::ExposureProgram program;
        for (const auto &trade : trades)
        {
            trade->bind(grid);
            ASSERT_TRUE(trade->compile(program));
        }
        ASSERT_EQ(program.trades.size(), trades.size());
        ASSERT_EQ(program.dates, grid.size());
        const xva::ProgramView view = program.view();

        // A random walk wide enough to exercise some swaptions and not others.
        const int n = static_cast<int>(grid.size());
        std::vector<Real> state(grid.size());
        std::size_t exercised_paths = 0, compared = 0;
        for (std::uint64_t p = 0; p < 64; ++p)
        {
            PhiloxGaussianSource gaussian(7, p);
            Real x = 0.0;
            Time previous = 0.0;
            for (std::size_t i = 0; i < grid.size(); ++i)
            {
                x += kSigma * std::sqrt(grid[i] - previous) * gaussian.next();
                state[i] = x;
                previous = grid[i];
            }
            for (std::size_t k = 0; k < trades.size(); ++k)
            {
                const xva::ProgramTrade &trade = program.trades[k];
                const bool exercised = xva::program_exercised(view, trade, state.data());
                if (trade.expiry >= 0 && exercised)
                    ++exercised_paths;
                for (int i = 0; i < n; ++i)
                {
                    const std::size_t date = static_cast<std::size_t>(i);
                    const Real value = trades[k]->value(date, state.data());
                    const Real flow = trades[k]->cashflow(date, state.data());
                    // The same sums in the same order: the last bits at most.
                    EXPECT_NEAR(xva::program_value(view, trade, i, state.data(), exercised), value,
                                1e-12 * (1.0 + std::abs(value)))
                        << "trade " << k << ", date " << i;
                    EXPECT_NEAR(xva::program_cashflow(view, trade, i, state.data(), exercised), flow,
                                1e-12 * (1.0 + std::abs(flow)))
                        << "trade " << k << ", date " << i;
                    ++compared;
                }
            }
        }
        EXPECT_GT(compared, 10000u);
        // Both branches of the gate were taken.
        EXPECT_GT(exercised_paths, 20u);
        EXPECT_LT(exercised_paths, 108u);
    }

    TEST(ExposureProgram, ATradeValuedByRegressionHasNoProgram)
    {
        const HullWhiteCurveModel m = model();
        const auto bermudan =
            make_hull_white_future_value(BermudanSwaption{swap(m, 1.0, 5.0, true), {1.0, 2.0, 3.0}}, m);
        xva::ExposureProgram program;
        EXPECT_FALSE(bermudan->compile(program));
        EXPECT_TRUE(program.trades.empty());
        EXPECT_TRUE(program.date_records.empty());
    }

    TEST(ExposureProgram, TheCollateralPlanIsTheOneOfTheCube)
    {
        Csa csa;
        csa.threshold_counterparty = 2e5;
        csa.minimum_transfer_amount = 5e4;
        csa.margin_period_of_risk = 0.04;
        const std::vector<Time> times{0.02, 0.04, 0.06, 0.08, 0.10};
        const CollateralPlan plan = collateral_plan(times, csa, 3.5e5);
        // 0.02 and 0.04 look back to today; 0.06 to 0.02, 0.08 to 0.04, 0.10 to 0.06.
        EXPECT_EQ(plan.lagged, (std::vector<int>{CollateralPlan::kToday, CollateralPlan::kToday, 0, 1, 2}));
        EXPECT_EQ(plan.reporting, (std::vector<int>{0, 1, 2, 3, 4}));
        EXPECT_EQ(plan.is_call_date, (std::vector<unsigned char>{1, 1, 1, 0, 0}));
        // Today's call: 350 000 - 200 000 of threshold, above the MTA.
        EXPECT_DOUBLE_EQ(plan.held_today, 1.5e5);
        // A grid without the lagged dates is refused, as collateralise() does.
        EXPECT_THROW(collateral_plan({0.5, 1.0}, csa, 0.0), InvalidInput);
    }

    TEST(ExposureProgram, ANettingSetWithoutItsCubeIsTheOneReadOffTheCube)
    {
        const HullWhiteCurveModel m = model();
        HullWhiteExposureEngine engine(m);
        fill(engine, m);
        Csa csa;
        csa.threshold_counterparty = 1e5;
        csa.threshold_bank = 2e5;
        csa.minimum_transfer_amount = 2.5e4;
        csa.rounding = 1e3;
        csa.independent_amount = 1e4;

        const CreditCurve counterparty(0.02), own(0.01);
        NettingSetRequest request;
        request.trades = {0, 2, 3};
        request.csa = csa;
        request.collateral.cashflows = MarginPeriodCashflows::Withheld;
        request.weights = [&](const std::vector<Time> &times, std::vector<Real> &on_positive,
                              std::vector<Real> &on_negative)
        {
            on_positive = first_to_default_weights(times, counterparty, own);
            on_negative = first_to_default_weights(times, own, counterparty);
            for (Real &w : on_positive)
                w *= -0.6;
            for (Real &w : on_negative)
                w *= -0.6;
        };
        const NettingSetExposure direct = engine.simulate_netting_set(settings(2000), request);
        EXPECT_EQ(direct.device, "cpu");
        EXPECT_EQ(direct.gpus, 0);

        ExposureSimulationSettings s = settings(2000);
        s.grid.margin_period_of_risk = csa.margin_period_of_risk;
        const ExposurePaths cube = engine.simulate(s);
        const ExposurePaths netted = collateralise(cube, csa, request.trades, request.collateral);
        const ExposureStatistics expected = exposure_statistics(netted);
        ASSERT_EQ(direct.statistics.times, expected.times);
        EXPECT_EQ(direct.statistics.discounted_ee, expected.discounted_ee);
        EXPECT_EQ(direct.statistics.discounted_ene, expected.discounted_ene);
        EXPECT_EQ(direct.statistics.pfe, expected.pfe);
        EXPECT_DOUBLE_EQ(direct.statistics.value_today, expected.value_today);
        EXPECT_EQ(direct.statistics.trades, request.trades);

        // The mean of the per-path sums is the integral of the profiles: the
        // CVA and the DVA of risk/xva.hpp.
        Real cva = 0.0, dva = 0.0;
        const std::vector<Real> pd_c = first_to_default_weights(expected.times, counterparty, own);
        const std::vector<Real> pd_i = first_to_default_weights(expected.times, own, counterparty);
        for (std::size_t r = 0; r < expected.times.size(); ++r)
        {
            cva -= 0.6 * pd_c[r] * expected.discounted_ee[r];
            dva -= 0.6 * pd_i[r] * expected.discounted_ene[r];
        }
        EXPECT_LT(cva, 0.0);
        EXPECT_GT(dva, 0.0);
        EXPECT_NEAR(direct.weighted_positive.value, cva, 1e-9 * std::abs(cva));
        EXPECT_NEAR(direct.weighted_negative.value, dva, 1e-9 * std::abs(dva));
        EXPECT_GT(direct.weighted_positive.error, 0.0);
        EXPECT_LT(direct.weighted_positive.error, 0.2 * std::abs(cva));
    }

    TEST(ExposureProgram, WithoutACsaTheNettingSetIsThePlainSum)
    {
        const HullWhiteCurveModel m = model();
        HullWhiteExposureEngine engine(m);
        fill(engine, m);
        const NettingSetExposure direct = engine.simulate_netting_set(settings(1000), {});
        const ExposureStatistics expected = exposure_statistics(engine.simulate(settings(1000)));
        ASSERT_EQ(direct.statistics.times, expected.times);
        for (std::size_t i = 0; i < expected.times.size(); ++i)
        {
            EXPECT_NEAR(direct.statistics.discounted_ee[i], expected.discounted_ee[i],
                        1e-9 * (1.0 + expected.discounted_ee[i]));
            EXPECT_NEAR(direct.statistics.pfe[i], expected.pfe[i], 1e-9 * (1.0 + expected.pfe[i]));
        }
        EXPECT_NEAR(direct.statistics.eepe, expected.eepe, 1e-9 * expected.eepe);
    }

    TEST(ExposureProgram, WhatTheNettingSetCannotDoWithoutTheCubeIsRefused)
    {
        const HullWhiteCurveModel m = model();
        HullWhiteExposureEngine engine(m);
        fill(engine, m);
        NettingSetRequest margin_without_csa;
        margin_without_csa.collateral.initial_margin_received = {1.0};
        EXPECT_THROW(engine.simulate_netting_set(settings(100), margin_without_csa), InvalidInput);
        NettingSetRequest pathwise;
        pathwise.csa = Csa{};
        pathwise.collateral.initial_margin_received_paths = {1.0};
        EXPECT_THROW(engine.simulate_netting_set(settings(100), pathwise), InvalidInput);
        NettingSetRequest twice;
        twice.trades = {1, 1};
        EXPECT_THROW(engine.simulate_netting_set(settings(100), twice), InvalidInput);
        NettingSetRequest out_of_range;
        out_of_range.trades = {4};
        EXPECT_THROW(engine.simulate_netting_set(settings(100), out_of_range), InvalidInput);
    }

    TEST(ExposureProgram, WithoutADeviceAutoRunsOnTheCpuAndSaysWhy)
    {
        if (gpu::device_count() > 0)
            GTEST_SKIP() << "this build has a CUDA device: see tests/gpu/testGpuExposure.cpp";
        const HullWhiteCurveModel m = model();
        HullWhiteExposureEngine engine(m);
        fill(engine, m);
        ExposureSimulationSettings s = settings(200);
        const ExposurePaths cpu = engine.simulate(s);
        EXPECT_EQ(cpu.device, "cpu");
        EXPECT_TRUE(cpu.device_note.empty());

        s.device = ComputeDevice::Auto;
        const ExposurePaths automatic = engine.simulate(s);
        EXPECT_EQ(automatic.device, "cpu");
        EXPECT_FALSE(automatic.device_note.empty());
        EXPECT_EQ(automatic.trade_values, cpu.trade_values);
        EXPECT_FALSE(engine.simulate_netting_set(s, {}).device_note.empty());

        s.device = ComputeDevice::Gpu;
        EXPECT_THROW(engine.simulate(s), gpu::GpuUnavailable);
        EXPECT_THROW(engine.simulate_netting_set(s, {}), gpu::GpuUnavailable);
    }

} // namespace quantModeling
