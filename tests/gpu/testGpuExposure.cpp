#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "quantModeling/engines/analytic/swap.hpp"
#include "quantModeling/engines/xva/exposure_engine.hpp"
#include "quantModeling/gpu/device.hpp"
#include "quantModeling/risk/collateral.hpp"
#include "quantModeling/risk/xva_report.hpp"

// Lot X7 of blueprint/wp/23-xva.md §14.11: the exposure engine and the
// collateral on the device. The CPU engine is the oracle -- same Philox
// draws, same sums, so the two agree to the last bits of exp and erfc, far
// below any Monte-Carlo error -- and one card or two give the same bits.

namespace quantModeling
{
    namespace
    {
        constexpr Real kNotional = 1e7;

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

        HullWhiteCurveModel model()
        {
            return HullWhiteCurveModel(0.03, 0.01, curve(0.035, 0.004), curve(0.038, 0.005));
        }

        InterestRateSwap swap(const HullWhiteCurveModel &m, Time start, Time tenor, bool payer,
                              Real moneyness = 0.0, Real spread = 0.0)
        {
            const Real par = value_swap(make_swap(start, tenor, 0.0, 1, 2),
                                        MultiCurve{m.discount(), m.projection()})
                                 .par_rate;
            return make_swap(start, tenor, par + moneyness, 1, 2, kNotional, payer, spread);
        }

        void fill(HullWhiteExposureEngine &engine, const HullWhiteCurveModel &m)
        {
            engine.add(swap(m, 0.0, 10.0, true));
            engine.add(swap(m, 1.0, 6.0, false, 0.004, 0.001), 2.0);
            engine.add(Swaption{swap(m, 2.0, 5.0, true, 0.002), 2.0});
            engine.add(Swaption{swap(m, 3.0, 4.0, false, -0.001), 3.0}, -1.5);
        }

        ExposureSimulationSettings settings(std::size_t paths, ComputeDevice device, int max_gpus = 1)
        {
            ExposureSimulationSettings s;
            s.paths = paths;
            s.seed = 2026;
            s.keep_cashflows = true;
            s.device = device;
            s.max_gpus = max_gpus;
            return s;
        }

        /// Largest |a - b| over two matrices.
        Real gap(const std::vector<Real> &a, const std::vector<Real> &b)
        {
            EXPECT_EQ(a.size(), b.size());
            Real worst = 0.0;
            for (std::size_t j = 0; j < std::min(a.size(), b.size()); ++j)
                worst = std::max(worst, std::abs(a[j] - b[j]));
            return worst;
        }

        /// Values of a book of 1e7 of notional agree to a millionth of a
        /// currency unit: fourteen digits.
        constexpr Real kValueTolerance = 1e-6;

        Csa csa()
        {
            Csa c;
            c.threshold_counterparty = 1e5;
            c.threshold_bank = 2e5;
            c.minimum_transfer_amount = 2.5e4;
            c.rounding = 1e3;
            c.independent_amount = 1e4;
            return c;
        }

        NettingSetRequest request(bool collateralised, MarginPeriodCashflows cashflows = MarginPeriodCashflows::Paid)
        {
            NettingSetRequest r;
            r.trades = {0, 2, 3};
            if (collateralised)
            {
                r.csa = csa();
                r.collateral.cashflows = cashflows;
            }
            r.weights = [](const std::vector<Time> &times, std::vector<Real> &on_positive,
                           std::vector<Real> &on_negative)
            {
                const CreditCurve counterparty(0.02), own(0.01);
                on_positive = first_to_default_weights(times, counterparty, own);
                on_negative = first_to_default_weights(times, own, counterparty);
                for (Real &w : on_positive)
                    w *= -0.6;
                for (Real &w : on_negative)
                    w *= -0.6;
            };
            return r;
        }

        void expect_same_profiles(const NettingSetExposure &gpu, const NettingSetExposure &cpu)
        {
            const ExposureStatistics &g = gpu.statistics, &c = cpu.statistics;
            ASSERT_EQ(g.times, c.times);
            const auto near = [](const std::vector<Real> &a, const std::vector<Real> &b, const char *what)
            {
                ASSERT_EQ(a.size(), b.size()) << what;
                for (std::size_t i = 0; i < a.size(); ++i)
                    EXPECT_NEAR(a[i], b[i], 1e-9 * (1.0 + std::abs(b[i]))) << what << ", date " << i;
            };
            near(g.discounted_ee, c.discounted_ee, "EE*");
            near(g.discounted_ene, c.discounted_ene, "ENE*");
            near(g.discounted_efv, c.discounted_efv, "EFV*");
            near(g.discounted_ee_error, c.discounted_ee_error, "error of EE*");
            near(g.discounted_ene_error, c.discounted_ene_error, "error of ENE*");
            near(g.ee, c.ee, "EE");
            EXPECT_NEAR(g.epe, c.epe, 1e-9 * c.epe);
            EXPECT_NEAR(g.eepe, c.eepe, 1e-9 * c.eepe);
            EXPECT_NEAR(g.value_today, c.value_today, 1e-9 * (1.0 + std::abs(c.value_today)));
            EXPECT_EQ(g.trades, c.trades);
            EXPECT_NEAR(gpu.weighted_positive.value, cpu.weighted_positive.value,
                        1e-9 * std::abs(cpu.weighted_positive.value));
            EXPECT_NEAR(gpu.weighted_negative.value, cpu.weighted_negative.value,
                        1e-9 * std::abs(cpu.weighted_negative.value));
            EXPECT_NEAR(gpu.weighted_positive.error, cpu.weighted_positive.error,
                        1e-9 * cpu.weighted_positive.error);
            EXPECT_NEAR(gpu.weighted_negative.error, cpu.weighted_negative.error,
                        1e-9 * cpu.weighted_negative.error);
        }

        class GpuExposureTest : public ::testing::Test
        {
          protected:
            void SetUp() override
            {
                if (gpu::device_count() == 0)
                    GTEST_SKIP() << "no CUDA device";
            }
        };
    } // namespace

    TEST_F(GpuExposureTest, TheCubeOfTheDeviceIsTheCubeOfTheCpu)
    {
        const HullWhiteCurveModel m = model();
        HullWhiteExposureEngine engine(m);
        fill(engine, m);
        // Three logical blocks, the last one short.
        const std::size_t paths = 10000;
        const ExposurePaths cpu = engine.simulate(settings(paths, ComputeDevice::Cpu));
        const ExposurePaths gpu = engine.simulate(settings(paths, ComputeDevice::Gpu));
        EXPECT_EQ(gpu.device, "gpu");
        EXPECT_EQ(gpu.gpus, 1);
        EXPECT_EQ(gpu.times, cpu.times);
        EXPECT_EQ(gpu.discount, cpu.discount);
        EXPECT_EQ(gpu.trade_values_today, cpu.trade_values_today);
        EXPECT_LT(gap(gpu.discount_weight, cpu.discount_weight), 1e-13);
        ASSERT_EQ(gpu.trades(), cpu.trades());
        ASSERT_EQ(gpu.trade_cashflows.size(), cpu.trade_cashflows.size());
        for (std::size_t k = 0; k < cpu.trades(); ++k)
        {
            EXPECT_LT(gap(gpu.trade_values[k], cpu.trade_values[k]), kValueTolerance) << "trade " << k;
            EXPECT_LT(gap(gpu.trade_cashflows[k], cpu.trade_cashflows[k]), kValueTolerance) << "trade " << k;
            // Not a cube of zeros.
            EXPECT_GT(*std::max_element(cpu.trade_values[k].begin(), cpu.trade_values[k].end()), 1e3);
        }
    }

    TEST_F(GpuExposureTest, TheHistoricalScenariosAreTheCpusToo)
    {
        const HullWhiteCurveModel m = model();
        HullWhiteExposureEngine engine(m);
        fill(engine, m);
        ExposureSimulationSettings s = settings(5000, ComputeDevice::Cpu);
        s.historical = HistoricalRateDynamics{0.25, 0.03, 0.012};
        const ExposurePaths cpu = engine.simulate(s);
        s.device = ComputeDevice::Gpu;
        const ExposurePaths gpu = engine.simulate(s);
        EXPECT_EQ(gpu.measure, ExposureMeasure::Historical);
        EXPECT_EQ(gpu.discount_weight, cpu.discount_weight); // all ones
        for (std::size_t k = 0; k < cpu.trades(); ++k)
            EXPECT_LT(gap(gpu.trade_values[k], cpu.trade_values[k]), kValueTolerance) << "trade " << k;
    }

    TEST_F(GpuExposureTest, TheCubeDoesNotDependOnTheLaunchesNorOnTheCards)
    {
        const HullWhiteCurveModel m = model();
        HullWhiteExposureEngine engine(m);
        fill(engine, m);
        const std::size_t paths = 5 * 4096 - 77;
        const ExposurePaths one = engine.simulate(settings(paths, ComputeDevice::Gpu));
        ExposureSimulationSettings small = settings(paths, ComputeDevice::Gpu);
        small.gpu_blocks_per_launch = 2;
        const ExposurePaths launches = engine.simulate(small);
        EXPECT_EQ(launches.discount_weight, one.discount_weight);
        EXPECT_EQ(launches.trade_values, one.trade_values);
        EXPECT_EQ(launches.trade_cashflows, one.trade_cashflows);

        if (gpu::device_count() < 2)
            GTEST_SKIP() << "one CUDA device only";
        const ExposurePaths two = engine.simulate(settings(paths, ComputeDevice::Gpu, 2));
        EXPECT_EQ(two.gpus, 2);
        EXPECT_EQ(two.discount_weight, one.discount_weight);
        EXPECT_EQ(two.trade_values, one.trade_values);
        EXPECT_EQ(two.trade_cashflows, one.trade_cashflows);
    }

    TEST_F(GpuExposureTest, TheNettingSetOnTheDeviceIsTheCpuPipeline)
    {
        const HullWhiteCurveModel m = model();
        HullWhiteExposureEngine engine(m);
        fill(engine, m);
        const std::size_t paths = 12000;
        Real open = 0.0, with_csa = 0.0;
        for (const bool collateralised : {false, true})
        {
            const NettingSetRequest r = request(collateralised);
            const NettingSetExposure cpu =
                engine.simulate_netting_set(settings(paths, ComputeDevice::Cpu), r);
            const NettingSetExposure gpu =
                engine.simulate_netting_set(settings(paths, ComputeDevice::Gpu), r);
            EXPECT_EQ(gpu.device, "gpu");
            EXPECT_EQ(gpu.gpus, 1);
            // No quantile without the paths.
            EXPECT_TRUE(gpu.statistics.pfe.empty());
            expect_same_profiles(gpu, cpu);
            (collateralised ? with_csa : open) = gpu.statistics.epe;
        }
        // The CSA does something: the exposure is a fraction of the open one.
        EXPECT_GT(open, 1e5);
        EXPECT_LT(with_csa, 0.6 * open);
    }

    TEST_F(GpuExposureTest, TheFlowsOfTheMarginPeriodAndTheInitialMarginAreTheCpus)
    {
        const HullWhiteCurveModel m = model();
        HullWhiteExposureEngine engine(m);
        fill(engine, m);
        const std::size_t paths = 6000;
        for (const MarginPeriodCashflows cashflows :
             {MarginPeriodCashflows::Withheld, MarginPeriodCashflows::OnlyBankPays})
        {
            NettingSetRequest r = request(true, cashflows);
            // An initial margin on both sides, decaying over the reporting dates.
            ExposureSimulationSettings grid_settings = settings(paths, ComputeDevice::Cpu);
            grid_settings.grid.margin_period_of_risk = r.csa->margin_period_of_risk;
            const std::vector<Time> grid = engine.grid(grid_settings.grid);
            const std::size_t m_dates = margin_periods(grid, r.csa->margin_period_of_risk).size();
            for (std::size_t j = 0; j < m_dates; ++j)
            {
                const Real left = 1.0 - static_cast<Real>(j) / static_cast<Real>(m_dates);
                r.collateral.initial_margin_received.push_back(6e4 * left);
                r.collateral.initial_margin_posted.push_back(4e4 * left);
            }
            const NettingSetExposure cpu =
                engine.simulate_netting_set(settings(paths, ComputeDevice::Cpu), r);
            const NettingSetExposure gpu =
                engine.simulate_netting_set(settings(paths, ComputeDevice::Gpu), r);
            expect_same_profiles(gpu, cpu);
        }
    }

    TEST_F(GpuExposureTest, TheProfilesAreTheSameBitsOnOneCardOrTwo)
    {
        const HullWhiteCurveModel m = model();
        HullWhiteExposureEngine engine(m);
        fill(engine, m);
        const std::size_t paths = 6 * 4096 - 1234;
        const NettingSetRequest r = request(true, MarginPeriodCashflows::Withheld);
        const NettingSetExposure one = engine.simulate_netting_set(settings(paths, ComputeDevice::Gpu), r);
        const auto expect_same_bits = [&one](const NettingSetExposure &other)
        {
            EXPECT_EQ(other.statistics.discounted_ee, one.statistics.discounted_ee);
            EXPECT_EQ(other.statistics.discounted_ene, one.statistics.discounted_ene);
            EXPECT_EQ(other.statistics.discounted_efv, one.statistics.discounted_efv);
            EXPECT_EQ(other.statistics.discounted_ee_error, one.statistics.discounted_ee_error);
            EXPECT_EQ(other.statistics.eepe, one.statistics.eepe);
            EXPECT_EQ(other.weighted_positive.value, one.weighted_positive.value);
            EXPECT_EQ(other.weighted_positive.error, one.weighted_positive.error);
            EXPECT_EQ(other.weighted_negative.value, one.weighted_negative.value);
            EXPECT_EQ(other.weighted_negative.error, one.weighted_negative.error);
        };
        ExposureSimulationSettings small = settings(paths, ComputeDevice::Gpu);
        small.gpu_blocks_per_launch = 1;
        expect_same_bits(engine.simulate_netting_set(small, r));
        small.gpu_blocks_per_launch = 4;
        expect_same_bits(engine.simulate_netting_set(small, r));

        if (gpu::device_count() < 2)
            GTEST_SKIP() << "one CUDA device only";
        const NettingSetExposure two = engine.simulate_netting_set(settings(paths, ComputeDevice::Gpu, 2), r);
        EXPECT_EQ(two.gpus, 2);
        expect_same_bits(two);
    }

    TEST_F(GpuExposureTest, WhatTheDeviceCannotValueStaysOnTheCpuAndSaysSo)
    {
        const HullWhiteCurveModel m = model();
        HullWhiteExposureEngine engine(m);
        fill(engine, m);

        // SIMM on every path: computed on the CPU.
        ExposureSimulationSettings with_simm = settings(500, ComputeDevice::Auto);
        with_simm.simm = true;
        const ExposurePaths margined = engine.simulate(with_simm);
        EXPECT_EQ(margined.device, "cpu");
        EXPECT_NE(margined.device_note.find("SIMM"), std::string::npos);
        with_simm.device = ComputeDevice::Gpu;
        EXPECT_THROW(engine.simulate(with_simm), InvalidInput);

        // Left free, a book the device can value goes there.
        const ExposurePaths automatic = engine.simulate(settings(500, ComputeDevice::Auto));
        EXPECT_EQ(automatic.device, "gpu");
        EXPECT_TRUE(automatic.device_note.empty());

        // A Bermudan is valued by regression.
        engine.add(BermudanSwaption{swap(m, 1.0, 5.0, true), {1.0, 2.0, 3.0}});
        const ExposurePaths regressed = engine.simulate(settings(500, ComputeDevice::Auto));
        EXPECT_EQ(regressed.device, "cpu");
        EXPECT_NE(regressed.device_note.find("regression"), std::string::npos);
        EXPECT_THROW(engine.simulate(settings(500, ComputeDevice::Gpu)), InvalidInput);
        EXPECT_THROW(engine.simulate_netting_set(settings(500, ComputeDevice::Gpu), {}), InvalidInput);
    }

} // namespace quantModeling
