// Compiled scripts against the bytecode interpreter on the GPU
// (blueprint/wp/26-performance.md §5, lot P1b).
//
// The four products of P0's GPU reference paths (benchmarks/reference/), as
// benchmarks/perf_matrix.py writes them, priced by the script engine on one
// V100 with Philox and antithetic pairs -- the configuration P0 measured at
// 2.0x to 4.8x the hand-written reference. For each: the interpreter
// (QM_SCRIPT_JIT=0) and the compiled script, same paths, same bits; the
// median of five timed runs after a warm-up (the warm-up pays the
// compilation, reported separately).
//
//   build-cuda/qm_script_jit_bench [paths_scale=1] [device=0] [only=<case>] [sampler=philox|sobol]
//
// Default path counts: those qm_gpu_reference_bench reaches a relative
// standard error of 1e-3 with, so that the times compare to its own.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "quantModeling/core/date.hpp"
#include "quantModeling/engines/mc/script_engine.hpp"
#include "quantModeling/gpu/device.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/market/valuation_context.hpp"
#include "quantModeling/scripting/script_model_factory.hpp"

#include "reference/reference_scripts.hpp"

using namespace quantModeling;

namespace
{
    const Date kValuation = Date::from_iso("2026-01-01");

    std::string weekly_barrier()
    {
        const std::string check = "    if spot() >= 130 then alive = 0 endIf\n";
        std::string s = (kValuation + 7).to_iso() + "\n    alive = 1\n" + check + "\n";
        for (int k = 2; k < 52; ++k)
            s += (k > 2 ? "  " : "") + (kValuation + 7 * k).to_iso();
        s += "\n" + check + "\n" + (kValuation + 7 * 52).to_iso() + "\n" + check +
             "    if alive = 1 then pays max(spot() - 100, 0) endIf\n";
        return s;
    }

    const char *kDailyAsian = "2026-01-02\n    acc = spot()\n    n = 1\n\n"
                              "schedule(2026-01-05, 2026-12-31, 1D, TARGET, F)\n    acc = acc + spot()\n"
                              "    n = n + 1\n\n"
                              "2027-01-01\n    acc = acc + spot()\n    n = n + 1\n"
                              "    pays max(acc / n - 100, 0)\n";

    scripting::ScriptModelSpec spec(bool local_vol)
    {
        scripting::ScriptModelSpec s;
        s.spot = 100.0;
        s.rate = 0.03;
        s.dividend = 0.0;
        s.vol = 0.2;
        s.max_dt = 1.0 / 52.0;
        if (local_vol)
        {
            s.model = "local_vol";
            for (int i = 0; i < 17; ++i)
                s.K_grid.push_back(40.0 + 10.0 * i);
            s.T_grid = {0.1, 0.25, 0.5, 0.75, 1.0, 1.5, 2.0};
            for (double k : s.K_grid)
                for (double t : s.T_grid)
                    s.sigma_loc_flat.push_back(std::max(0.08, 0.2 - 0.12 * std::log(k / 100.0) + 0.01 * t));
        }
        return s;
    }

    struct Case
    {
        const char *name;
        std::string script;
        bool local_vol;
        int paths;
        bool three_assets = false; ///< the worst-of autocall's model
    };

    struct Timing
    {
        double npv = 0.0, se = 0.0, warm_ms = 0.0, median_ms = 0.0;
        std::string diagnostics;
    };

    bool g_sobol = false; // Sobol RQMC (16 replicates, Brownian bridge) instead of Philox + antithetic

    Timing run(const Case &c, bool compiled, int paths, int device)
    {
        setenv("QM_SCRIPT_JIT", compiled ? "1" : "0", 1);
        const ScriptedProduct<Real> product(c.script, ValuationContext{kValuation});
        auto model = scripting::make_script_model<Real>(c.three_assets ? qm_reference::worst_of_autocall_model(0.03)
                                                                       : spec(c.local_vol));
        PricingSettings s;
        s.mc_paths = paths;
        s.mc_seed = 1;
        s.mc_antithetic = true;
        s.mc_rng = RngKind::Philox;
        s.mc_sampler = g_sobol ? SamplerKind::Sobol : SamplerKind::PseudoRandom; // explicit: Sobol is the default
        s.mc_device = ComputeDevice::Gpu;
        s.mc_gpus = 1; // one V100, as the reference
        (void)device;
        Timing t;
        auto once = [&]()
        {
            const auto t0 = std::chrono::steady_clock::now();
            const SimulationMCResult r = simulate_script(product, *model, s);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            t.npv = r.npv();
            t.se = r.std_error();
            t.diagnostics = r.diagnostics;
            return ms;
        };
        t.warm_ms = once();
        std::vector<double> ms;
        for (int k = 0; k < 5; ++k)
            ms.push_back(once());
        std::sort(ms.begin(), ms.end());
        t.median_ms = ms[2];
        return t;
    }
} // namespace

int main(int argc, char **argv)
{
    const double scale = argc > 1 ? std::atof(argv[1]) : 1.0;
    const int device = argc > 2 ? std::atoi(argv[2]) : 0;
    const std::string only = argc > 3 ? argv[3] : "";
    g_sobol = argc > 4 && std::string(argv[4]) == "sobol";
    if (gpu::device_count() == 0)
    {
        std::puts("no CUDA device");
        return 1;
    }
    const std::vector<Case> cases = {
        {"barrier_bs", weekly_barrier(), false, 2261592},
        {"barrier_lv", weekly_barrier(), true, 1877726},
        {"asian_bs", kDailyAsian, false, 1163724},
        {"asian_lv", kDailyAsian, true, 950442},
        {"autocall_bs", qm_reference::worst_of_autocall_script(kValuation), false, 2000000, true},
    };
    std::printf("Script engine on one V100, %s: interpreter vs compiled script\n",
                g_sobol ? "Sobol RQMC (16 replicates, bridge)" : "Philox + antithetic");
    std::printf("%-11s %9s  %12s %12s %8s  %10s  %s\n", "case", "paths", "interp (ms)", "compiled (ms)", "speedup",
                "compile ms", "same bits");
    for (const Case &c : cases)
    {
        if (!only.empty() && only != c.name)
            continue;
        const int paths = static_cast<int>(c.paths * scale);
        const Timing a = run(c, false, paths, device);
        const Timing b = run(c, true, paths, device);
        const bool same = std::memcmp(&a.npv, &b.npv, sizeof a.npv) == 0 && std::memcmp(&a.se, &b.se, sizeof a.se) == 0;
        std::printf("%-11s %9d  %12.2f %12.2f %7.2fx  %10.0f  %s  (npv %.6f, se %.2e)\n", c.name, paths, a.median_ms,
                    b.median_ms, a.median_ms / b.median_ms, b.warm_ms - b.median_ms, same ? "yes" : "NO", b.npv, b.se);
        if (b.diagnostics.find("compiled script") == std::string::npos)
            std::printf("    not compiled: %s\n", b.diagnostics.c_str());
    }
    unsetenv("QM_SCRIPT_JIT");
    return 0;
}
