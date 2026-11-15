#ifndef QM_BENCHMARKS_REFERENCE_CASES_HPP
#define QM_BENCHMARKS_REFERENCE_CASES_HPP

// The cases of the reference benchmarks and their time-to-error driver, on
// the host. The terms are benchmarks/perf_matrix.py's.

#include "reference_paths.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace qm_reference
{

    struct Case
    {
        std::string name;
        Product product;
        Terms terms;
        bool local_vol;
    };

    struct Grid
    {
        std::vector<double> K, T, sigma;
    };

    /// benchmarks/perf_matrix.py's grid: strikes 40..200, seven maturities.
    inline Grid local_vol_grid()
    {
        Grid g;
        for (int i = 0; i <= 16; ++i)
            g.K.push_back(40.0 + 10.0 * i);
        g.T = {0.1, 0.25, 0.5, 0.75, 1.0, 1.5, 2.0};
        for (double k : g.K)
            for (double t : g.T)
                g.sigma.push_back(std::max(0.08, 0.2 - 0.12 * std::log(k / 100.0) + 0.01 * t));
        return g;
    }

    inline std::vector<Case> cases()
    {
        const double r = 0.03, T_weekly = 364.0 / 365.0;
        const Terms barrier{100.0, r, 0.0, 0.2, 100.0, 130.0, T_weekly, 52, std::exp(-r * T_weekly)};
        const Terms asian{100.0, r, 0.0, 0.2, 100.0, 0.0, 1.0, 252, std::exp(-r)};
        const Terms autocall{100.0, r, 0.0, 0.25, 0.0, 0.0, 2.0, 8, 0.0}; // 8 quarters; payments discounted at their dates
        return {{"barrier_bs  up-and-out, 52 weekly dates (BS)", Product::UpAndOut, barrier, false},
                {"barrier_lv  up-and-out, 52 weekly dates (local vol)", Product::UpAndOut, barrier, true},
                {"asian_bs    arithmetic Asian, 252 fixings (BS)", Product::Asian, asian, false},
                {"asian_lv    arithmetic Asian, 252 fixings (local vol)", Product::Asian, asian, true},
                {"autocall_bs worst-of autocall, 3 assets, 8 quarters (BS)", Product::WorstOfAutocall, autocall,
                 false}};
    }

    struct Estimate
    {
        double mean = 0.0, se = 0.0, seconds = 0.0;
    };

    /// run(units) prices `units` antithetic pairs. A pilot fixes the number of
    /// units for the target relative standard error; the timed run is the
    /// median of three.
    inline void time_to_error(const std::string &label, double target,
                              const std::function<Estimate(uint64_t)> &run)
    {
        run(1 << 10); // warm-up
        const uint64_t pilot = 1 << 14;
        const Estimate p = run(pilot);
        const double rel = p.se / std::fabs(p.mean);
        const auto units = static_cast<uint64_t>(std::ceil(pilot * (rel / target) * (rel / target) * 1.02));
        std::vector<Estimate> runs;
        for (int k = 0; k < 3; ++k)
            runs.push_back(run(units));
        std::sort(runs.begin(), runs.end(), [](const Estimate &a, const Estimate &b)
                  { return a.seconds < b.seconds; });
        const Estimate &m = runs[1];
        std::printf("  %-52s %9.4f s   npv %.5f   rel se %.2e   paths %llu\n", label.c_str(), m.seconds, m.mean,
                    m.se / std::fabs(m.mean), static_cast<unsigned long long>(2 * units));
    }

    template <class F>
    double seconds(F &&f)
    {
        const auto t0 = std::chrono::steady_clock::now();
        f();
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }

} // namespace qm_reference

#endif // QM_BENCHMARKS_REFERENCE_CASES_HPP
