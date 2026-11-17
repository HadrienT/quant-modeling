// Benchmark of blueprint/wp/23-xva.md §14.11 (lot X7): wall time for the CVA
// of a collateralised netting set to reach a target relative standard error,
// CPU against GPU, on the same Philox scenarios.
//
//   build-cuda/qm_gpu_exposure_bench [target_rel_se=0.01] [cpu_threads=16] [large_paths=1000000]
//                                    [contribution_paths=50000]
//
// A pilot run estimates the dispersion of the per-path CVA, which fixes the
// number of paths; every row then runs those paths and prints the error it
// actually reached -- a speed-up quoted without its standard error means
// nothing. A second table runs a number of paths whose cube no host would
// hold, which only the reduction on the device can do. A third (lot G1,
// #281 step 2) times the trades' stand-alone and incremental CVA: the host's
// xva_report() on the cube the card sends back, against the 2K subsets
// reduced in the card's own launch.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "quantModeling/engines/analytic/swap.hpp"
#include "quantModeling/engines/xva/exposure_engine.hpp"
#include "quantModeling/gpu/device.hpp"
#include "quantModeling/risk/collateral.hpp"
#include "quantModeling/risk/xva_report.hpp"
#include "quantModeling/utils/accumulators.hpp"
#include "quantModeling/utils/thread_pool.hpp"

namespace qm = quantModeling;

namespace
{
    using Clock = std::chrono::steady_clock;

    template <class F>
    double seconds(F &&f)
    {
        const auto t0 = Clock::now();
        f();
        return std::chrono::duration<double>(Clock::now() - t0).count();
    }

    qm::DiscountCurve curve(double level, double slope)
    {
        std::vector<qm::Time> times;
        std::vector<qm::Real> dfs;
        for (int k = 1; k <= 160; ++k)
        {
            const qm::Time t = 0.25 * k;
            times.push_back(t);
            dfs.push_back(std::exp(-(level + slope * (1.0 - std::exp(-t / 5.0))) * t));
        }
        return qm::DiscountCurve(times, dfs, qm::CurveExtrapolation::FlatForward);
    }

    qm::InterestRateSwap swap(const qm::HullWhiteCurveModel &m, qm::Time start, qm::Time tenor, bool payer,
                              double notional, double moneyness = 0.0)
    {
        const double par = qm::value_swap(qm::make_swap(start, tenor, 0.0, 1, 2),
                                          qm::MultiCurve{m.discount(), m.projection()})
                               .par_rate;
        return qm::make_swap(start, tenor, par + moneyness, 1, 2, notional, payer);
    }

    /// A directional book of ten trades: eight swaps from 2 to 20 years, more
    /// paid than received, and two swaptions.
    void fill(qm::HullWhiteExposureEngine &engine, const qm::HullWhiteCurveModel &m)
    {
        engine.add(swap(m, 0.0, 2.0, true, 5e7));
        engine.add(swap(m, 0.0, 5.0, true, 4e7, 0.002));
        engine.add(swap(m, 0.0, 7.0, false, 3e7, -0.001));
        engine.add(swap(m, 0.0, 10.0, true, 5e7));
        engine.add(swap(m, 1.0, 10.0, true, 2e7, 0.003));
        engine.add(swap(m, 0.0, 15.0, false, 2e7));
        engine.add(swap(m, 0.0, 20.0, true, 3e7, -0.002));
        engine.add(swap(m, 2.0, 8.0, false, 2e7));
        engine.add(qm::Swaption{swap(m, 3.0, 7.0, true, 4e7, 0.002), 3.0});
        engine.add(qm::Swaption{swap(m, 5.0, 10.0, false, 3e7), 5.0}, -1.0);
    }

    qm::Csa csa()
    {
        qm::Csa c;
        c.minimum_transfer_amount = 1e5;
        c.rounding = 1e4;
        c.margin_period_of_risk = 10.0 / 250.0;
        return c;
    }

    const qm::CreditCurve kCounterparty(0.02), kOwn(0.01);
    constexpr double kLgd = 0.6;
    // The bank's funding spreads and what the CSA pays over the discount rate.
    constexpr double kBorrowing = 0.012, kLending = 0.007, kCollateralSpread = 0.003;

    void weights(const std::vector<qm::Time> &times, std::vector<qm::Real> &on_positive,
                 std::vector<qm::Real> &on_negative)
    {
        on_positive = qm::first_to_default_weights(times, kCounterparty, kOwn);
        on_negative = qm::first_to_default_weights(times, kOwn, kCounterparty);
        for (qm::Real &w : on_positive)
            w *= -kLgd;
        for (qm::Real &w : on_negative)
            w *= -kLgd;
    }

    qm::NettingSetRequest request()
    {
        qm::NettingSetRequest r;
        r.csa = csa();
        r.weights = weights;
        return r;
    }

    qm::ExposureSimulationSettings settings(std::size_t paths, qm::ComputeDevice device, int gpus = 1)
    {
        qm::ExposureSimulationSettings s;
        s.paths = paths;
        s.seed = 20261002;
        s.device = device;
        s.max_gpus = gpus;
        // The CPU rows hold the cube; the server leaves about 20 GiB to one
        // computation.
        s.memory_limit_bytes = std::size_t{12} << 30;
        return s;
    }

    /// CVA of the cube the GPU sent back, post-processed on the host as the
    /// report does.
    qm::Estimate cva_of_cube(const qm::ExposurePaths &cube)
    {
        const qm::ExposurePaths netted = qm::collateralise(cube, csa());
        std::vector<qm::Real> a, b;
        weights(netted.times, a, b);
        const std::size_t m = netted.dates();
        qm::WelfordAccumulator cva;
        for (std::size_t p = 0; p < netted.paths; ++p)
        {
            double loss = 0.0;
            for (std::size_t r = 0; r < m; ++r)
            {
                const double v = netted.discount_weight[p * m + r] * netted.trade_values[0][p * m + r];
                if (v > 0.0)
                    loss += a[r] * v;
            }
            cva.add(loss);
        }
        return {cva.mean, cva.std_error()};
    }

    struct Row
    {
        std::string label;
        double secs = 0.0;
        qm::Estimate cva;
    };

    void print(const Row &row, double reference)
    {
        std::printf("  %-28s %9.3f s   CVA %12.2f   std err %9.2f (%.2e relative)   x%.0f\n", row.label.c_str(),
                    row.secs, row.cva.value, row.cva.error, row.cva.error / std::abs(row.cva.value),
                    reference / row.secs);
    }
} // namespace

int main(int argc, char **argv)
{
    const double target = argc > 1 ? std::atof(argv[1]) : 0.01;
    const int cpu_threads = argc > 2 ? std::atoi(argv[2]) : 16;
    const std::size_t large = argc > 3 ? static_cast<std::size_t>(std::atof(argv[3])) : 1000000;
    const std::size_t contribution_paths = argc > 4 ? static_cast<std::size_t>(std::atof(argv[4])) : 50000;

    const qm::HullWhiteCurveModel model(0.03, 0.01, curve(0.035, 0.004), curve(0.038, 0.005));
    qm::HullWhiteExposureEngine engine(model);
    fill(engine, model);
    const qm::NettingSetRequest netting = request();
    const int gpus = qm::gpu::device_count();

    // Pilot: the dispersion of the per-path CVA.
    const qm::NettingSetExposure pilot = engine.simulate_netting_set(
        settings(8192, gpus > 0 ? qm::ComputeDevice::Gpu : qm::ComputeDevice::Cpu, 2), netting);
    const double rel_sd = pilot.weighted_positive.error * std::sqrt(8192.0) / std::abs(pilot.weighted_positive.value);
    const auto paths = static_cast<std::size_t>(std::ceil(rel_sd * rel_sd / (target * target)));
    qm::ExposureSimulationSettings grid_settings = settings(paths, qm::ComputeDevice::Cpu);
    grid_settings.grid.margin_period_of_risk = csa().margin_period_of_risk;
    const std::size_t dates = engine.grid(grid_settings.grid).size();

    std::printf("Netting set of %zu trades (8 swaps of 2 to 20 years, 2 swaptions), CSA with a 10-day margin\n"
                "period of risk, %zu dates, Hull-White one factor, Philox pseudo-random.\n",
                engine.trades(), dates);
    std::printf("Target relative std err of the CVA %.1e -> %zu paths (cube: %.2f GiB on the host)\n\n", target,
                paths, static_cast<double>(paths * dates * (engine.trades() + 1) * sizeof(double)) / (1u << 30));

    std::vector<Row> rows;
    const auto run_netting = [&](const std::string &label, const qm::ExposureSimulationSettings &s,
                                 qm::ThreadPool *pool)
    {
        Row row;
        row.label = label;
        qm::NettingSetExposure out;
        row.secs = seconds([&]
                           { out = engine.simulate_netting_set(s, netting, pool); });
        row.cva = out.weighted_positive;
        rows.push_back(row);
        print(row, rows.front().secs);
        return out;
    };

    const qm::NettingSetExposure cpu1 = run_netting("CPU 1 thread", settings(paths, qm::ComputeDevice::Cpu), nullptr);
    {
        qm::ThreadPool pool;
        pool.start(static_cast<std::size_t>(cpu_threads - 1)); // the caller is the last thread
        run_netting("CPU " + std::to_string(cpu_threads) + " threads", settings(paths, qm::ComputeDevice::Cpu), &pool);
        pool.stop();
    }

    if (gpus == 0)
    {
        std::printf("  (no CUDA device: GPU rows skipped)\n");
        return 0;
    }
    {
        Row row;
        row.label = "1 V100, cube to the host";
        qm::ExposureSimulationSettings s = settings(paths, qm::ComputeDevice::Gpu);
        s.grid.margin_period_of_risk = csa().margin_period_of_risk;
        double simulate = 0.0;
        row.secs = seconds([&]
                           {
                               qm::ExposurePaths cube;
                               simulate = seconds([&]
                                                  { cube = engine.simulate(s); });
                               row.cva = cva_of_cube(cube); });
        rows.push_back(row);
        print(row, rows.front().secs);
        std::printf("  %-28s %9.3f s   (the rest is the host: collateral and statistics, one thread)\n",
                    "    of which the cube", simulate);
    }
    const qm::NettingSetExposure gpu1 =
        run_netting("1 V100, reduced on the card", settings(paths, qm::ComputeDevice::Gpu, 1), nullptr);
    if (gpus > 1)
        run_netting("2 V100, reduced on the cards", settings(paths, qm::ComputeDevice::Gpu, 2), nullptr);

    std::printf("\n  CVA: CPU %.17g, GPU %.17g -- |GPU - CPU| / std err = %.2e\n", cpu1.weighted_positive.value,
                gpu1.weighted_positive.value,
                std::abs(gpu1.weighted_positive.value - cpu1.weighted_positive.value) / cpu1.weighted_positive.error);

    std::printf("\n| | time | CVA | std err | speed-up |\n|---|---|---|---|---|\n");
    for (const Row &row : rows)
        std::printf("| %s | %.3f s | %.0f | %.0f | x%.0f |\n", row.label.c_str(), row.secs, row.cva.value,
                    row.cva.error, rows.front().secs / row.secs);

    // What only the device can do: paths whose cube the host could not hold.
    std::printf("\n%zu paths (the cube would take %.0f GiB on the host):\n", large,
                static_cast<double>(large * dates * (engine.trades() + 1) * sizeof(double)) / (1u << 30));
    // The profiles and the CVA alone: at this size the sorted paths the PFE
    // needs would be gigabytes to bring home (lot G3).
    qm::NettingSetRequest profiles = netting;
    profiles.distribution = false;
    for (int g = 1; g <= std::min(gpus, 2); ++g)
    {
        qm::NettingSetExposure out;
        const double secs = seconds([&]
                                    { out = engine.simulate_netting_set(settings(large, qm::ComputeDevice::Gpu, g),
                                                                        profiles); });
        std::printf("  %d V100   %8.3f s   CVA %12.2f   std err %9.2f (%.2e relative)   %.2e paths/s\n", g, secs,
                    out.weighted_positive.value, out.weighted_positive.error,
                    out.weighted_positive.error / std::abs(out.weighted_positive.value),
                    static_cast<double>(large) / secs);
    }

    // Lot G1: the trades' contributions.
    std::printf("\nThe %zu trades' stand-alone and incremental CVA, %zu paths:\n", engine.trades(),
                contribution_paths);
    qm::XvaReport report;
    {
        qm::ThreadPool pool;
        pool.start(static_cast<std::size_t>(cpu_threads - 1));
        qm::ExposureSimulationSettings s = settings(contribution_paths, qm::ComputeDevice::Gpu);
        s.grid.margin_period_of_risk = csa().margin_period_of_risk;
        qm::XvaInputs in;
        in.csa = csa();
        in.counterparty = kCounterparty;
        in.own = kOwn;
        in.lgd_counterparty = kLgd;
        in.lgd_own = kLgd;
        in.borrowing_spread = kBorrowing;
        in.lending_spread = kLending;
        in.collateral_spread = kCollateralSpread;
        double cube_secs = 0.0;
        const double secs = seconds(
            [&]
            {
                qm::ExposurePaths cube;
                cube_secs = seconds([&]
                                    { cube = engine.simulate(s); });
                report = qm::xva_report(cube, in, &pool);
            });
        pool.stop();
        std::printf("  %-44s %8.3f s   (cube %.3f s, xva_report on %d threads %.3f s)\n",
                    "1 V100 cube + host xva_report", secs, cube_secs, cpu_threads, secs - cube_secs);
    }
    qm::NettingSetRequest contributions = netting;
    contributions.contributions = true;
    contributions.distribution = false;
    for (int g = 1; g <= std::min(gpus, 2); ++g)
    {
        qm::NettingSetExposure out;
        const double secs = seconds(
            [&]
            { out = engine.simulate_netting_set(settings(contribution_paths, qm::ComputeDevice::Gpu, g),
                                                contributions); });
        double worst = 0.0;
        for (std::size_t j = 0; j < out.contributions.size(); ++j)
            worst = std::max({worst, std::abs(out.contributions[j].standalone_cva - report.contributions[j].standalone_cva),
                              std::abs(out.contributions[j].incremental_cva - report.contributions[j].incremental_cva)});
        std::printf("  %d V100, %-35s %8.3f s   worst |card - host| / |CVA| = %.1e\n", g, "reduced on the card(s)",
                    secs, worst / std::abs(report.cva.value));
        if (g == 1)
        {
            std::printf("    trade  stand-alone CVA (err)        incremental CVA (err)\n");
            for (const qm::TradeContribution &c : out.contributions)
                std::printf("    %5zu  %12.2f (%8.2f)   %12.2f (%8.2f)\n", c.trade, c.standalone_cva,
                            c.standalone_cva_error, c.incremental_cva, c.incremental_cva_error);
        }
    }

    // #281 step 3: FCA, FBA, FVA and ColVA as more sums of the same launch.
    qm::NettingSetRequest running = contributions;
    running.running_weights = [](const std::vector<qm::Time> &times, std::vector<qm::Real> &funding_positive,
                                 std::vector<qm::Real> &funding_negative, std::vector<qm::Real> &collateral)
    {
        funding_positive = qm::running_cost_weights(times, kCounterparty, kOwn, kBorrowing);
        funding_negative = qm::running_cost_weights(times, kCounterparty, kOwn, kLending);
        collateral = qm::running_cost_weights(times, kCounterparty, kOwn, kCollateralSpread);
    };
    std::printf("\nWith FCA, FBA, FVA and ColVA in the same launch, %zu paths:\n", contribution_paths);
    for (int g = 1; g <= std::min(gpus, 2); ++g)
    {
        qm::NettingSetExposure out;
        const double secs = seconds(
            [&]
            { out = engine.simulate_netting_set(settings(contribution_paths, qm::ComputeDevice::Gpu, g), running); });
        const double worst = std::max({std::abs(out.funding_positive.value - report.fca) / std::abs(report.fca),
                                       std::abs(out.funding_negative.value - report.fba) / std::abs(report.fba),
                                       std::abs(out.weighted_collateral.value - report.colva) /
                                           std::abs(report.colva)});
        std::printf("  %d V100, %-35s %8.3f s   worst relative |card - host| = %.1e\n", g,
                    "contributions + running costs", secs, worst);
        if (g == 1)
            std::printf("    FCA %.2f (%.2f)  FBA %.2f (%.2f)  FVA %.2f (%.2f)  ColVA %.2f (%.2f)\n"
                        "    host: FCA %.2f (%.2f)  FBA %.2f (%.2f)  FVA %.2f (%.2f)  ColVA %.2f (%.2f)\n",
                        out.funding_positive.value, out.funding_positive.error, out.funding_negative.value,
                        out.funding_negative.error, out.funding.value, out.funding.error,
                        out.weighted_collateral.value, out.weighted_collateral.error, report.fca, report.fca_error,
                        report.fba, report.fba_error, report.fca + report.fba, report.fva_error, report.colva,
                        report.colva_error);
    }

    // #281 step 4: initial margin per path, the same netting set. The host
    // fits the margin on the cube it reads; the card on a pilot of its own.
    constexpr double kMarginSpread = 0.009;
    std::printf("\nUnder a dynamic initial margin (FCA, FBA, ColVA and MVA too), %zu paths:\n", contribution_paths);
    qm::XvaReport margined_report;
    {
        qm::ThreadPool pool;
        pool.start(static_cast<std::size_t>(cpu_threads - 1));
        qm::ExposureSimulationSettings s = settings(contribution_paths, qm::ComputeDevice::Gpu);
        s.grid.margin_period_of_risk = csa().margin_period_of_risk;
        s.keep_cashflows = true;
        qm::XvaInputs in;
        in.csa = csa();
        in.counterparty = kCounterparty;
        in.own = kOwn;
        in.lgd_counterparty = kLgd;
        in.lgd_own = kLgd;
        in.borrowing_spread = kBorrowing;
        in.lending_spread = kLending;
        in.collateral_spread = kCollateralSpread;
        in.initial_margin = qm::DimSettings{};
        in.initial_margin_spread = kMarginSpread;
        double cube_secs = 0.0;
        const double secs = seconds(
            [&]
            {
                qm::ExposurePaths cube;
                cube_secs = seconds([&]
                                    { cube = engine.simulate(s); });
                margined_report = qm::xva_report(cube, in, &pool);
            });
        pool.stop();
        std::printf("  %-44s %8.3f s   (cube %.3f s, xva_report on %d threads %.3f s)\n",
                    "1 V100 cube + host xva_report", secs, cube_secs, cpu_threads, secs - cube_secs);
        std::printf("    host: CVA %.2f (%.2f)  MVA %.2f (%.2f)  IM today %.0f\n", margined_report.cva.value,
                    margined_report.cva.error, margined_report.mva, margined_report.mva_error,
                    margined_report.initial_margin_today);
    }
    qm::NettingSetRequest margined = running;
    margined.contributions = false;
    margined.margin_weights = [](const std::vector<qm::Time> &times, std::vector<qm::Real> &on_margin)
    { on_margin = qm::running_cost_weights(times, kCounterparty, kOwn, kMarginSpread); };
    qm::ThreadPool fit_pool;
    fit_pool.start(static_cast<std::size_t>(cpu_threads - 1));
    for (int g = 1; g <= std::min(gpus, 2); ++g)
    {
        qm::NettingSetExposure out;
        double fit_secs = 0.0;
        const double secs = seconds(
            [&]
            {
                const qm::ExposureSimulationSettings s = settings(contribution_paths, qm::ComputeDevice::Gpu, g);
                fit_secs = seconds([&]
                                   { margined.margin_model = engine.fit_initial_margin(s, csa(), {}, {}, 0, &fit_pool); });
                out = engine.simulate_netting_set(s, margined);
            });
        std::printf("  %d V100, %-35s %8.3f s   (pilot fit %.3f s, %d threads)\n", g, "pilot fit + reduced on the card(s)",
                    secs, fit_secs, cpu_threads);
        if (g == 1)
            std::printf("    card: CVA %.2f (%.2f)  MVA %.2f (%.2f)  IM today %.0f  (another fit of the margin: "
                        "a gap of a few errors is expected)\n",
                        out.weighted_positive.value, out.weighted_positive.error, out.weighted_margin.value,
                        out.weighted_margin.error, out.initial_margin_today);
    }
    fit_pool.stop();
    return 0;
}
