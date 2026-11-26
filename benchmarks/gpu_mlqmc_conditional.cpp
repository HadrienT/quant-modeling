// The multilevel QMC corrections of step quantities on the GPU, with the
// coarse path conditioned on the fine one (WP 26 §26.2, issue #363) and
// without, on fixed levels: each level's mean and V_l (the variance of its
// mean over the replicates, times the points per replicate, so that the
// levels compare). The CPU's conditioned levels are the oracle: the same
// Sobol points on level 0, and above it the same estimator on another
// ordering of the fine variates (the GPU's fine bridge is nested on the
// coarse one, mc::coupled_sobol_tables), so the levels agree within their
// errors, not to the bit.
//
//   qm_gpu_mlqmc_conditional_bench [points per replicate] [finest level] [cpu]
//
// Local vol on tests/testMlmc.cpp's skewed 5 x 3 surface, level 0 at
// monthly steps, 32 replicates. With "cpu", the CPU's conditioned levels
// too (minutes at the default sizes).

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "quantModeling/core/date.hpp"
#include "quantModeling/engines/mc/mlqmc.hpp"
#include "quantModeling/engines/mc/script_engine.hpp"
#include "quantModeling/gpu/device.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/market/valuation_context.hpp"
#include "quantModeling/scripting/script_model_factory.hpp"

namespace qm = quantModeling;

namespace
{
    qm::scripting::ScriptModelSpec skewed_local_vol()
    {
        qm::scripting::ScriptModelSpec s;
        s.model = "local_vol";
        s.spot = 100.0;
        s.rate = 0.05;
        s.dividend = 0.02;
        s.K_grid = {50.0, 80.0, 100.0, 120.0, 200.0};
        s.T_grid = {0.0, 0.5, 2.0};
        s.sigma_loc_flat = {0.80, 0.70, 0.60, 0.45, 0.40, 0.35, 0.25, 0.24, 0.22,
                            0.16, 0.15, 0.14, 0.10, 0.10, 0.10};
        s.max_dt = 1.0 / 12.0;
        // QM_BENCH_GRID: a market grid from a file (qm_mlmc_conditional_bench's
        // format: dividend, nK nT, strikes scaled to a spot of 100, maturities,
        // sigma_loc strike-major). QM_BENCH_SCHEME=before: Milstein on every
        // step that reaches no date (#371).
        if (const char *path = std::getenv("QM_BENCH_GRID"))
        {
            std::ifstream in(path);
            std::size_t nK = 0, nT = 0;
            in >> s.dividend >> nK >> nT;
            s.rate = 0.04;
            s.K_grid.resize(nK), s.T_grid.resize(nT), s.sigma_loc_flat.resize(nK * nT);
            for (double &k : s.K_grid)
                in >> k;
            for (double &t : s.T_grid)
                in >> t;
            for (double &v : s.sigma_loc_flat)
                in >> v;
            if (!in)
                throw std::runtime_error(std::string("QM_BENCH_GRID: cannot read ") + path);
        }
        if (const char *scheme = std::getenv("QM_BENCH_SCHEME"))
            s.milstein_before_dates = std::string(scheme) == "before";
        return s;
    }

    /// log2 slope over levels 1..L, least squares.
    double slope(const std::vector<double> &y)
    {
        double n = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (std::size_t l = 1; l < y.size(); ++l)
            if (y[l] > 0.0)
            {
                const double x = static_cast<double>(l), v = std::log2(y[l]);
                n += 1, sx += x, sy += v, sxx += x * x, sxy += x * v;
            }
        return n < 2 ? 0.0 : (n * sxy - sx * sy) / (n * sxx - sx * sx);
    }
} // namespace

int main(int argc, char **argv)
{
    const long long points = argc > 1 ? std::atoll(argv[1]) : 4096;
    const int finest = argc > 2 ? std::atoi(argv[2]) : 6;
    const bool with_cpu = argc > 3 && std::string(argv[3]) == "cpu";
    if (qm::gpu::device_count() == 0)
    {
        std::fprintf(stderr, "no CUDA device\n");
        return 1;
    }
    const qm::ValuationContext ctx{qm::Date::from_iso("2025-12-31")};
    const std::pair<const char *, const char *> cases[] = {
        {"digital K=100, pays 100, lastStepDigital", "2027-01-01\n    pays 100 * lastStepDigital(100)\n"},
        {"vanilla call K=100, lastStepCall", "2027-01-01\n    pays lastStepCall(100)\n"},
        {"up-and-out call K=100 B=130, survivalBelow",
         "2027-01-01\n    s = survivalBelow(130)\n    pays s * max(spot() - 100, 0)\n"},
        // #369 L4: daily closes, hard and with the step into each close
        // conditioned on survival (level 0 then takes daily steps)
        {"up-and-out call K=100 B=130, daily closes, hard",
         "2026-01-01\n    alive = 1\nschedule(2026-01-02, 2026-12-31, 1D, US, F)\n"
         "    if spot() >= 130 then alive = 0 endIf\n2027-01-01\n"
         "    if spot() >= 130 then alive = 0 endIf\n    if alive = 1 then pays max(spot() - 100, 0) endIf\n"},
        {"up-and-out call K=100 B=130, daily closes, knockOutAbove",
         "2026-01-01\n    k = 100\nschedule(2026-01-02, 2026-12-31, 1D, US, F)\n    knockOutAbove(130)\n"
         "2027-01-01\n    knockOutAbove(130)\n    pays max(spot() - k, 0)\n"},
    };
    const char *only = std::getenv("QM_BENCH_ONLY"); // a substring of the cases' names
    std::printf("Local vol (skewed 5x3), multilevel QMC, 32 replicates x %lld points a level, levels 0..%d\n",
                points, finest);
    for (const auto &[name, script] : cases)
    {
        if (only && std::string(name).find(only) == std::string::npos)
            continue;
        const qm::ScriptedProduct<qm::Real> product(script, ctx);
        auto model = qm::scripting::make_script_model<qm::Real>(skewed_local_vol());
        qm::MlqmcSettings s;
        s.seed = 11;
        std::vector<std::vector<qm::QmcLevelStats>> runs;
        std::vector<double> secs;
        const char *labels[] = {"GPU, coarse on its own steps", "GPU, coarse conditioned", "CPU, coarse conditioned"};
        for (int k = 0; k < (with_cpu ? 3 : 2); ++k)
        {
            s.coarse_conditioning = k != 0;
            const auto t0 = std::chrono::steady_clock::now();
            runs.push_back(qm::mlqmc_fixed_levels(product, *model, finest, points, s, k != 2));
            secs.push_back(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        }
        std::printf("\n%s\n", name);
        for (std::size_t k = 0; k < runs.size(); ++k)
        {
            std::vector<double> v, m;
            double cost = 0.0, value = 0.0, units = 0.0;
            for (const qm::QmcLevelStats &lv : runs[k])
            {
                v.push_back(lv.estimator_variance * static_cast<double>(lv.points));
                m.push_back(std::fabs(lv.mean));
                cost += std::sqrt(v.back() * lv.cost);
                value += lv.mean;
                units += 32.0 * static_cast<double>(lv.points) * lv.cost;
            }
            // the optimal cost in seconds: what this run's device took per
            // model step, times the steps the optimal allocation needs at
            // unit variance (compare it between estimators of one product)
            std::printf("  %-30s value %.5f   beta %.2f   optimal cost %.4g   (%.1f s; %.4g s at unit variance)\n",
                        labels[k], value, -slope(v), cost * cost, secs[k], cost * cost * secs[k] / units);
            std::printf("    V_l:   ");
            for (const double x : v)
                std::printf(" %10.3e", x);
            std::printf("\n    E Y_l: ");
            for (const double x : m)
                std::printf(" %10.3e", x);
            std::printf("\n");
        }
        if (runs.size() == 3)
        {
            double gap = 0.0;
            for (std::size_t l = 0; l < runs[1].size(); ++l)
                gap = std::max(gap, std::fabs(runs[1][l].mean - runs[2][l].mean) /
                                        std::max(1e-300, std::sqrt(runs[1][l].estimator_variance +
                                                                   runs[2][l].estimator_variance)));
            std::printf("  GPU against CPU, conditioned: largest level gap %.2f of the level's standard error "
                        "(the same estimator on other draws above level 0)\n",
                        gap);
        }
    }
    return 0;
}
