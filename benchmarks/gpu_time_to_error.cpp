// Benchmark of blueprint/wp/19-gpu.md §9: wall time to reach a target
// relative standard error, CPU against GPU, on the same units, the same
// Philox draws and the same reduction tree.
//
//   build-cuda/qm_gpu_bench [target_rel_se=1e-4] [cpu_threads=8]
//
// A pilot run estimates the per-unit variance, which fixes the number of
// units n = Var / (target * price)^2; every column then runs those n units.
// A speed-up quoted without its standard error means nothing, so each row
// prints the error it actually reached.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "quantModeling/engines/mc/kernels/vanilla_bs.hpp"
#include "quantModeling/engines/mc/logical_blocks.hpp"
#include "quantModeling/gpu/device.hpp"
#include "quantModeling/gpu/vanilla_bs.hpp"
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

    qm::mc::VanillaTerminalSpec atm_call_spec()
    {
        const double S0 = 100, K = 100, r = 0.05, q = 0.02, sigma = 0.20, T = 1.0, dT = 1.0 / 365.0;
        const double mu = r - q - 0.5 * sigma * sigma;
        qm::mc::VanillaTerminalSpec s;
        s.S0 = S0;
        s.K = K;
        s.sigma = sigma;
        s.T = T;
        s.sqrtT = std::sqrt(T);
        s.movedSpot = S0 * std::exp(mu * T);
        s.rootVariance = sigma * s.sqrtT;
        s.df = std::exp(-r * T);
        s.dS = 0.01 * S0;
        s.factor_up = 1.01;
        s.factor_dn = 0.99;
        s.theta_bump = dT;
        s.movedSpot_upT = S0 * std::exp(mu * (T + dT));
        s.movedSpot_dnT = S0 * std::exp(mu * (T - dT));
        s.rootVariance_upT = sigma * std::sqrt(T + dT);
        s.rootVariance_dnT = sigma * std::sqrt(T - dT);
        s.df_upT = std::exp(-r * (T + dT));
        s.df_dnT = std::exp(-r * (T - dT));
        return s;
    }

    void report(const char *column, double secs, const qm::WelfordAccumulator &payoff, double df)
    {
        const double price = df * payoff.mean;
        const double se = df * payoff.std_error();
        std::printf("  %-14s %9.3f s   price %.6f   std err %.2e (%.2e relative)\n", column, secs, price, se,
                    se / price);
    }
} // namespace

int main(int argc, char **argv)
{
    const double target = argc > 1 ? std::atof(argv[1]) : 1e-4;
    const int cpu_threads = argc > 2 ? std::atoi(argv[2]) : 8;

    const auto spec = atm_call_spec();
    using Unit = qm::mc::VanillaPhiloxUnit<qm::OptionType::Call, /*Antithetic=*/true, /*IS=*/false>;
    const Unit unit{spec, /*seed=*/20260926, 0.0};

    // Pilot: per-unit (antithetic pair) variance of the undiscounted payoff.
    const auto pilot = qm::mc::reduce_logical_blocks<qm::mc::VanillaStats>(uint64_t{1} << 20, unit);
    const double rel_sd = std::sqrt(pilot.payoff.variance()) / pilot.payoff.mean;
    const auto n_units = static_cast<uint64_t>(std::ceil(rel_sd * rel_sd / (target * target)));

    std::printf("Vanilla BS, ATM call, antithetic, Philox pseudo-random, all six estimators per path\n");
    std::printf("target relative std err %.1e -> %llu antithetic pairs (%.3g paths)\n\n", target,
                static_cast<unsigned long long>(n_units), 2.0 * static_cast<double>(n_units));

    qm::mc::VanillaStats cpu1, cpuN, gpu1;
    const double t_cpu1 = seconds([&]
                                  { cpu1 = qm::mc::reduce_logical_blocks<qm::mc::VanillaStats>(n_units, unit); });
    report("CPU 1 thread", t_cpu1, cpu1.payoff, spec.df);

    qm::ThreadPool pool;
    pool.start(static_cast<std::size_t>(cpu_threads - 1)); // the caller is the last thread
    const double t_cpuN = seconds([&]
                                  { cpuN = qm::mc::reduce_logical_blocks<qm::mc::VanillaStats>(n_units, unit, &pool); });
    pool.stop();
    const std::string cpuN_label = "CPU " + std::to_string(cpu_threads) + " threads";
    report(cpuN_label.c_str(), t_cpuN, cpuN.payoff, spec.df);

    double t_gpu1 = 0.0;
    if (qm::gpu::device_count() > 0)
    {
        qm::gpu::VanillaGpuRequest req;
        req.spec = spec;
        req.type = qm::OptionType::Call;
        req.antithetic = true;
        req.n_units = n_units;
        req.seed = unit.seed;
        // Warm-up: the CUDA context and module load happen once per process
        // and are not part of the pricing time.
        {
            auto warm = req;
            warm.n_units = 1;
            qm::gpu::simulate_vanilla_terminal(warm);
        }
        t_gpu1 = seconds([&]
                         { gpu1 = qm::gpu::simulate_vanilla_terminal(req); });
        report(("1 x " + qm::gpu::device_name(0)).substr(0, 14).c_str(), t_gpu1, gpu1.payoff, spec.df);
        std::printf("\n  |GPU - CPU| / std err = %.2e\n",
                    std::abs(gpu1.payoff.mean - cpu1.payoff.mean) / cpu1.payoff.std_error());
    }
    else
    {
        std::printf("  (no CUDA device: GPU column skipped)\n");
    }

    std::printf("\n| | CPU 1 thread | CPU %d threads | 1 V100 |\n|---|---|---|---|\n", cpu_threads);
    std::printf("| Vanilla BS | %.2f s | %.2f s | %s |\n", t_cpu1, t_cpuN,
                t_gpu1 > 0 ? (std::to_string(t_gpu1).substr(0, 5) + " s").c_str() : "n/a");
    return 0;
}
