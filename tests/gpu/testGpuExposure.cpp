#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "quantModeling/engines/analytic/swap.hpp"
#include "quantModeling/engines/xva/exposure_engine.hpp"
#include "quantModeling/gpu/device.hpp"
#include "quantModeling/risk/collateral.hpp"
#include "quantModeling/risk/xva_report.hpp"

#include "../support/cpu_gpu_tolerance.hpp"

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

    // Lot G3: the PFE, its error and further quantiles off the paths the
    // device sorted -- the CPU's order statistics (the values agree to the
    // CPU/GPU tolerance, so the selected ones do).
    TEST_F(GpuExposureTest, ThePfeAndTheQuantilesOfTheDeviceAreTheCpus)
    {
        const HullWhiteCurveModel m = model();
        HullWhiteExposureEngine engine(m);
        fill(engine, m);
        const std::size_t paths = 3 * 2048 + 777;
        for (const bool collateralised : {false, true})
        {
            NettingSetRequest r = request(collateralised);
            r.pfe_confidence = 0.99;
            r.quantile_levels = {0.01, 0.25, 0.5, 0.975};
            const NettingSetExposure cpu = engine.simulate_netting_set(settings(paths, ComputeDevice::Cpu), r);
            ExposureSimulationSettings launches = settings(paths, ComputeDevice::Gpu);
            launches.gpu_blocks_per_launch = 2; // several launches into one distribution
            const NettingSetExposure gpu = engine.simulate_netting_set(launches, r);
            ASSERT_EQ(gpu.device, "gpu") << gpu.device_note;
            const std::size_t n = cpu.statistics.times.size();
            ASSERT_EQ(gpu.statistics.pfe.size(), n);
            ASSERT_EQ(gpu.statistics.value_quantiles.size(), 4u);
            Real scale = 1.0, largest = 0.0;
            for (const Real x : cpu.statistics.pfe)
                scale = std::max(scale, std::abs(x));
            for (std::size_t i = 0; i < n; ++i)
            {
                EXPECT_NEAR(gpu.statistics.pfe[i], cpu.statistics.pfe[i], 1e-10 * scale) << "date " << i;
                EXPECT_NEAR(gpu.statistics.pfe_error[i], cpu.statistics.pfe_error[i], 1e-10 * scale) << "date " << i;
                for (std::size_t k = 0; k < 4; ++k)
                    EXPECT_NEAR(gpu.statistics.value_quantiles[k][i], cpu.statistics.value_quantiles[k][i],
                                1e-10 * scale)
                        << "date " << i << ", level " << k;
                largest = std::max(largest, cpu.statistics.pfe[i]);
            }
            EXPECT_GT(largest, 0.0);

            // On two cards: the same order statistics.
            if (gpu::device_count() >= 2)
            {
                const NettingSetExposure two =
                    engine.simulate_netting_set(settings(paths, ComputeDevice::Gpu, 2), r);
                EXPECT_EQ(two.gpus, 2);
                EXPECT_EQ(two.statistics.pfe, gpu.statistics.pfe);
                EXPECT_EQ(two.statistics.value_quantiles, gpu.statistics.value_quantiles);
            }
            // Not asked for: the profiles alone.
            r.distribution = false;
            EXPECT_TRUE(engine.simulate_netting_set(settings(paths, ComputeDevice::Gpu), r).statistics.pfe.empty());
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

    // Lot G1 (#281 step 2): the trades' stand-alone and incremental CVA, and
    // the Euler shares without a CSA, as 2K more netting sets of the same
    // launch. The CPU engine, itself held to xva_report()
    // (ExposureProgram.TheTradesContributionsAreThoseOfTheReport), is the
    // oracle.
    TEST_F(GpuExposureTest, TheTradesContributionsOnTheDeviceAreTheCpus)
    {
        const HullWhiteCurveModel m = model();
        HullWhiteExposureEngine engine(m);
        fill(engine, m);
        const std::size_t paths = 2 * 2048 + 999;
        struct Case
        {
            bool collateralised;
            MarginPeriodCashflows cashflows;
        };
        for (const Case c : {Case{false, MarginPeriodCashflows::Paid}, Case{true, MarginPeriodCashflows::Paid},
                             Case{true, MarginPeriodCashflows::Withheld},
                             Case{true, MarginPeriodCashflows::OnlyBankPays}})
        {
            NettingSetRequest r = request(c.collateralised, c.cashflows);
            r.trades = {0, 2, 3, 1};
            r.contributions = true;
            const NettingSetExposure cpu = engine.simulate_netting_set(settings(paths, ComputeDevice::Cpu), r);
            ExposureSimulationSettings launches = settings(paths, ComputeDevice::Gpu);
            launches.gpu_blocks_per_launch = 1; // several launches
            const NettingSetExposure gpu = engine.simulate_netting_set(launches, r);
            ASSERT_EQ(gpu.device, "gpu") << gpu.device_note;
            ASSERT_EQ(gpu.contributions.size(), r.trades.size());
            expect_same_profiles(gpu, cpu);
            // The CPU/GPU price tolerance, on the netting set's CVA.
            const Real scale = std::abs(cpu.weighted_positive.value);
            constexpr Real rel = test_support::kCpuGpuPriceRel;
            for (std::size_t j = 0; j < r.trades.size(); ++j)
            {
                const TradeContribution &g = gpu.contributions[j], &e = cpu.contributions[j];
                EXPECT_EQ(g.trade, e.trade);
                EXPECT_NEAR(g.standalone_cva, e.standalone_cva, rel * scale) << "trade " << j;
                EXPECT_NEAR(g.incremental_cva, e.incremental_cva, rel * scale) << "trade " << j;
                EXPECT_NEAR(g.standalone_cva_error, e.standalone_cva_error, rel * scale) << "trade " << j;
                EXPECT_NEAR(g.incremental_cva_error, e.incremental_cva_error, rel * scale) << "trade " << j;
                if (c.collateralised)
                    EXPECT_TRUE(std::isnan(g.marginal_cva));
                else
                {
                    EXPECT_NEAR(g.marginal_cva, e.marginal_cva, rel * scale) << "trade " << j;
                    EXPECT_NEAR(g.marginal_cva_error, e.marginal_cva_error, rel * scale) << "trade " << j;
                }
            }

            // Asking for them leaves the netting set's own figures alone, to
            // the bit.
            NettingSetRequest alone = r;
            alone.contributions = false;
            const NettingSetExposure plain = engine.simulate_netting_set(launches, alone);
            EXPECT_TRUE(plain.contributions.empty());
            EXPECT_EQ(plain.statistics.discounted_ee, gpu.statistics.discounted_ee);
            EXPECT_EQ(plain.weighted_positive.value, gpu.weighted_positive.value);
            EXPECT_EQ(plain.weighted_positive.error, gpu.weighted_positive.error);

            // The same bits on one launch or several, one card or two.
            const auto same_bits = [&gpu](const NettingSetExposure &other)
            {
                ASSERT_EQ(other.contributions.size(), gpu.contributions.size());
                for (std::size_t j = 0; j < gpu.contributions.size(); ++j)
                {
                    EXPECT_EQ(other.contributions[j].standalone_cva, gpu.contributions[j].standalone_cva);
                    EXPECT_EQ(other.contributions[j].incremental_cva, gpu.contributions[j].incremental_cva);
                    EXPECT_EQ(other.contributions[j].incremental_cva_error,
                              gpu.contributions[j].incremental_cva_error);
                    if (!std::isnan(gpu.contributions[j].marginal_cva))
                    {
                        EXPECT_EQ(other.contributions[j].marginal_cva, gpu.contributions[j].marginal_cva);
                    }
                }
            };
            same_bits(engine.simulate_netting_set(settings(paths, ComputeDevice::Gpu), r));
            if (gpu::device_count() >= 2)
            {
                const NettingSetExposure two = engine.simulate_netting_set(settings(paths, ComputeDevice::Gpu, 2), r);
                EXPECT_EQ(two.gpus, 2);
                same_bits(two);
            }
        }
    }

    // #281 step 3: FCA, FBA, FVA and ColVA as more per-path sums of the
    // netting set's launch, held to the CPU at the price tolerance.
    TEST_F(GpuExposureTest, TheRunningCostsAreSummedOnTheDeviceAsOnTheCpu)
    {
        const HullWhiteCurveModel m = model();
        HullWhiteExposureEngine engine(m);
        fill(engine, m);
        const std::size_t paths = 2 * 2048 + 999;
        const auto with_running = [](NettingSetRequest r)
        {
            r.running_weights = [](const std::vector<Time> &times, std::vector<Real> &funding_positive,
                                   std::vector<Real> &funding_negative, std::vector<Real> &collateral)
            {
                const CreditCurve counterparty(0.02), own(0.01);
                funding_positive = running_cost_weights(times, counterparty, own, 0.012);
                funding_negative = running_cost_weights(times, counterparty, own, 0.007);
                collateral = running_cost_weights(times, counterparty, own, 0.003);
            };
            return r;
        };
        struct Case
        {
            bool collateralised;
            MarginPeriodCashflows cashflows;
            bool margined;
            bool contributions;
        };
        for (const Case c : {Case{false, MarginPeriodCashflows::Paid, false, false},
                             Case{false, MarginPeriodCashflows::Paid, false, true},
                             Case{true, MarginPeriodCashflows::Paid, false, false},
                             Case{true, MarginPeriodCashflows::Withheld, true, false},
                             Case{true, MarginPeriodCashflows::OnlyBankPays, false, true}})
        {
            NettingSetRequest r = with_running(request(c.collateralised, c.cashflows));
            r.trades = {0, 2, 3, 1};
            r.contributions = c.contributions;
            if (c.margined)
            {
                // One value per reporting date: ask the CPU for the dates.
                const std::size_t m =
                    engine.simulate_netting_set(settings(16, ComputeDevice::Cpu), r).statistics.times.size();
                r.collateral.initial_margin_received.assign(m, 3e5);
                r.collateral.initial_margin_posted.assign(m, 2e5);
            }
            const NettingSetExposure cpu = engine.simulate_netting_set(settings(paths, ComputeDevice::Cpu), r);
            ExposureSimulationSettings launches = settings(paths, ComputeDevice::Gpu);
            launches.gpu_blocks_per_launch = 1; // several launches
            const NettingSetExposure gpu = engine.simulate_netting_set(launches, r);
            ASSERT_EQ(gpu.device, "gpu") << gpu.device_note;
            expect_same_profiles(gpu, cpu);

            constexpr Real rel = test_support::kCpuGpuPriceRel;
            const auto near = [](const Estimate &g, const Estimate &e, const char *what)
            {
                const Real scale = std::abs(e.value) + e.error;
                EXPECT_NEAR(g.value, e.value, rel * scale) << what;
                EXPECT_NEAR(g.error, e.error, rel * scale) << what;
            };
            near(gpu.funding_positive, cpu.funding_positive, "FCA");
            near(gpu.funding_negative, cpu.funding_negative, "FBA");
            near(gpu.funding, cpu.funding, "FVA");
            near(gpu.weighted_collateral, cpu.weighted_collateral, "ColVA");
            EXPECT_LT(gpu.funding_positive.value, 0.0);
            if (c.collateralised)
                EXPECT_NE(gpu.weighted_collateral.value, 0.0);
            else
                EXPECT_EQ(gpu.weighted_collateral.value, 0.0);

            // Asking for them leaves everything else alone, to the bit.
            NettingSetRequest plain = r;
            plain.running_weights = nullptr;
            const NettingSetExposure without = engine.simulate_netting_set(launches, plain);
            EXPECT_EQ(without.funding.value, 0.0);
            EXPECT_EQ(without.statistics.discounted_ee, gpu.statistics.discounted_ee);
            EXPECT_EQ(without.weighted_positive.value, gpu.weighted_positive.value);
            EXPECT_EQ(without.weighted_positive.error, gpu.weighted_positive.error);
            for (std::size_t j = 0; j < without.contributions.size(); ++j)
            {
                EXPECT_EQ(without.contributions[j].incremental_cva, gpu.contributions[j].incremental_cva);
            }

            // The same bits on one launch or several, one card or two.
            const auto same_bits = [&gpu](const NettingSetExposure &other)
            {
                EXPECT_EQ(other.funding_positive.value, gpu.funding_positive.value);
                EXPECT_EQ(other.funding_negative.value, gpu.funding_negative.value);
                EXPECT_EQ(other.funding.value, gpu.funding.value);
                EXPECT_EQ(other.funding.error, gpu.funding.error);
                EXPECT_EQ(other.weighted_collateral.value, gpu.weighted_collateral.value);
                EXPECT_EQ(other.weighted_collateral.error, gpu.weighted_collateral.error);
            };
            same_bits(engine.simulate_netting_set(settings(paths, ComputeDevice::Gpu), r));
            if (gpu::device_count() >= 2)
            {
                const NettingSetExposure two = engine.simulate_netting_set(settings(paths, ComputeDevice::Gpu, 2), r);
                EXPECT_EQ(two.gpus, 2);
                same_bits(two);
            }
        }
    }

    // #281 step 4 (lot G8): the dynamic initial margin evaluated on every
    // path in the kernel, held to the CPU's DynamicInitialMargin::margin()
    // on the cube, the same model on both sides.
    TEST_F(GpuExposureTest, TheInitialMarginOfEveryPathIsTheCpusModel)
    {
        const HullWhiteCurveModel m = model();
        HullWhiteExposureEngine engine(m);
        fill(engine, m);
        const std::size_t paths = 2 * 2048 + 999;
        const CreditCurve counterparty(0.02), own(0.01);
        for (const MarginPeriodCashflows cashflows : {MarginPeriodCashflows::Paid, MarginPeriodCashflows::OnlyBankPays})
        {
            NettingSetRequest r = request(true, cashflows);
            r.trades = {0, 2, 3, 1};
            DimSettings dim;
            dim.im_today = 4e5;
            r.margin_model =
                engine.fit_initial_margin(settings(paths, ComputeDevice::Cpu), *r.csa, r.trades, dim, 3000);
            r.margin_weights = [&](const std::vector<Time> &times, std::vector<Real> &on_margin)
            { on_margin = running_cost_weights(times, counterparty, own, 0.009); };
            r.running_weights = [&](const std::vector<Time> &times, std::vector<Real> &funding_positive,
                                    std::vector<Real> &funding_negative, std::vector<Real> &collateral)
            {
                funding_positive = running_cost_weights(times, counterparty, own, 0.012);
                funding_negative = running_cost_weights(times, counterparty, own, 0.007);
                collateral = running_cost_weights(times, counterparty, own, 0.003);
            };

            const NettingSetExposure cpu = engine.simulate_netting_set(settings(paths, ComputeDevice::Cpu), r);
            ExposureSimulationSettings launches = settings(paths, ComputeDevice::Gpu);
            launches.gpu_blocks_per_launch = 1; // several launches
            const NettingSetExposure gpu = engine.simulate_netting_set(launches, r);
            ASSERT_EQ(gpu.device, "gpu") << gpu.device_note;
            expect_same_profiles(gpu, cpu);

            constexpr Real rel = test_support::kCpuGpuPriceRel;
            ASSERT_EQ(gpu.discounted_expected_initial_margin.size(), cpu.discounted_expected_initial_margin.size());
            for (std::size_t i = 0; i < cpu.discounted_expected_initial_margin.size(); ++i)
            {
                const Real scale = cpu.discounted_expected_initial_margin[i];
                EXPECT_NEAR(gpu.discounted_expected_initial_margin[i], scale, rel * scale) << "date " << i;
                EXPECT_NEAR(gpu.discounted_expected_initial_margin_error[i],
                            cpu.discounted_expected_initial_margin_error[i], rel * scale)
                    << "date " << i;
                EXPECT_NEAR(gpu.expected_initial_margin[i], cpu.expected_initial_margin[i],
                            rel * cpu.expected_initial_margin[i])
                    << "date " << i;
            }
            EXPECT_EQ(gpu.initial_margin_today, cpu.initial_margin_today);
            const Real mva = std::abs(cpu.weighted_margin.value);
            EXPECT_NEAR(gpu.weighted_margin.value, cpu.weighted_margin.value, rel * mva);
            EXPECT_NEAR(gpu.weighted_margin.error, cpu.weighted_margin.error, rel * mva);
            EXPECT_LT(gpu.weighted_margin.value, 0.0);
            // Segregated margin funds nothing: the funding is the CPU's,
            // margin or not.
            const Real fva = std::abs(cpu.funding.value) + cpu.funding.error;
            EXPECT_NEAR(gpu.funding.value, cpu.funding.value, rel * fva);
            EXPECT_NEAR(gpu.weighted_positive.value, cpu.weighted_positive.value,
                        rel * std::abs(cpu.weighted_positive.value));

            // The same bits on one launch or several, one card or two.
            const auto same_bits = [&gpu](const NettingSetExposure &other)
            {
                EXPECT_EQ(other.discounted_expected_initial_margin, gpu.discounted_expected_initial_margin);
                EXPECT_EQ(other.weighted_margin.value, gpu.weighted_margin.value);
                EXPECT_EQ(other.weighted_margin.error, gpu.weighted_margin.error);
                EXPECT_EQ(other.statistics.discounted_ee, gpu.statistics.discounted_ee);
                EXPECT_EQ(other.weighted_positive.value, gpu.weighted_positive.value);
            };
            same_bits(engine.simulate_netting_set(settings(paths, ComputeDevice::Gpu), r));
            if (gpu::device_count() >= 2)
            {
                const NettingSetExposure two = engine.simulate_netting_set(settings(paths, ComputeDevice::Gpu, 2), r);
                EXPECT_EQ(two.gpus, 2);
                same_bits(two);
            }
        }

        // The pilot fitted on the device: the CPU's pilot, to the last bits
        // of the values (another device, the same draws).
        NettingSetRequest r = request(true);
        const DynamicInitialMargin on_cpu =
            engine.fit_initial_margin(settings(paths, ComputeDevice::Cpu), *r.csa, r.trades, {}, 3000);
        const DynamicInitialMargin on_gpu =
            engine.fit_initial_margin(settings(paths, ComputeDevice::Gpu), *r.csa, r.trades, {}, 3000);
        EXPECT_NEAR(on_gpu.today(), on_cpu.today(), 1e-9 * on_cpu.today());
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
