// The script engine on one CPU thread against the hand-written reference
// paths, at the same number of paths (blueprint/wp/26-performance.md §5,
// lots P1a / P1f).
//
// The four products of P0 (benchmarks/reference/), as perf_matrix.py writes
// them as scripts; Philox + antithetic on both sides, so that the draws cost
// the same and the difference is the engine. Median of five timed runs after
// a warm-up; time per antithetic unit (a pair of paths).
//
//   build/qm_script_cpu_bench [units=100000] [only=<case>]

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "quantModeling/core/date.hpp"
#include "quantModeling/engines/mc/script_engine.hpp"
#include "quantModeling/instruments/scripted_product.hpp"
#include "quantModeling/market/valuation_context.hpp"
#include "quantModeling/scripting/script_model_factory.hpp"

#include "reference/reference_cases.hpp"

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

    scripting::ScriptModelSpec spec(bool local_vol, const qm_reference::Grid &g)
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
            s.K_grid = g.K;
            s.T_grid = g.T;
            s.sigma_loc_flat = g.sigma;
        }
        return s;
    }

    template <class F>
    double median_ms(F &&f)
    {
        f(); // warm-up
        std::vector<double> ms;
        for (int k = 0; k < 5; ++k)
        {
            const auto t0 = std::chrono::steady_clock::now();
            f();
            ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        }
        std::sort(ms.begin(), ms.end());
        return ms[2];
    }
} // namespace

int main(int argc, char **argv)
{
    const long units = argc > 1 ? std::atol(argv[1]) : 100000;
    const std::string only = argc > 2 ? argv[2] : "";
    const qm_reference::Grid g = qm_reference::local_vol_grid();
    const qm_reference::LocalVolGrid view{g.K.data(), static_cast<int>(g.K.size()), g.T.data(),
                                          static_cast<int>(g.T.size()), g.sigma.data()};
    const std::vector<std::string> scripts = {weekly_barrier(), weekly_barrier(), kDailyAsian, kDailyAsian};
    const std::vector<qm_reference::Case> cases = qm_reference::cases();

    std::printf("One CPU thread, Philox + antithetic, %ld units (pairs of paths)\n", units);
    std::printf("%-11s %12s %12s %8s   %s\n", "case", "script (ms)", "ref (ms)", "ratio", "script ns/unit");
    for (std::size_t i = 0; i < cases.size(); ++i)
    {
        const qm_reference::Case &c = cases[i];
        const std::string name = c.name.substr(0, c.name.find(' '));
        if (!only.empty() && only != name)
            continue;
        const ScriptedProduct<Real> product(scripts[i], ValuationContext{kValuation});
        auto model = scripting::make_script_model<Real>(spec(c.local_vol, g));
        PricingSettings s;
        s.mc_paths = static_cast<int>(2 * units);
        s.mc_seed = 1;
        s.mc_antithetic = true;
        s.mc_rng = RngKind::Philox;
        s.mc_sampler = SamplerKind::PseudoRandom; // explicit: Sobol is the default
        s.mc_device = ComputeDevice::Cpu;
        double npv = 0.0;
        const double script_ms = median_ms([&]
                                           { npv = simulate_script(product, *model, s).npv(); });

        const qm_reference::LocalVolGrid *grid = c.local_vol ? &view : nullptr;
        double ref_sum = 0.0;
        const double ref_ms = median_ms(
            [&]
            {
                ref_sum = 0.0;
                for (long u = 0; u < units; ++u)
                    ref_sum += qm_reference::unit(c.product, c.terms, grid, 1, static_cast<uint64_t>(u));
            });
        std::printf("%-11s %12.1f %12.1f %7.2fx   %8.0f   (npv %.5f / %.5f)\n", name.c_str(), script_ms, ref_ms,
                    script_ms / ref_ms, 1e6 * script_ms / static_cast<double>(units), npv,
                    ref_sum / static_cast<double>(units));
    }
    return 0;
}
