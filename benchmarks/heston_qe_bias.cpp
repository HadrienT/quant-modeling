// Discretisation bias of Heston's variance schemes against the COS price
// (issue #317, acceptance 1): full-truncation Euler (what every price run
// used) against Andersen's QE-M (mc::qe_step), as a function of the steps a
// year, at Feller ratios 2κθ/ξ² of 1.5, 0.5, 0.18 and 0.13 (SPY's
// calibrated fit of 2026-11-18, the page's default market).
//
//     build/qm_heston_qe_bias [paths=4194304] [threads=16]
//
// Each case keeps SPY's v0, κ, θ, ρ and sets ξ for the ratio. One-year
// European options, priced on the same Philox draws for both schemes (draw 0
// and 1 of each step: Euler's spot and independent shocks, QE's variance and
// independent shocks). Prints, per scheme and step count, the bias
// (MC - COS) and its standard error, in % of the COS price.

#include "quantModeling/engines/analytic/heston_cos.hpp"
#include "quantModeling/models/equity/path_steps.hpp"
#include "quantModeling/utils/inverse_normal.hpp"
#include "quantModeling/utils/philox.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

using namespace quantModeling;

namespace
{
    struct Option
    {
        const char *name;
        double strike;
        bool call;
    };

    struct Sums
    {
        double s[2] = {0, 0}, s2[2] = {0, 0}; // per option
    };

    Sums run(const mc::HestonParamsT<double> &h, double r, double q, double S0, double T, int n_steps,
             mc::VarianceScheme scheme, const Option *opts, std::size_t first, std::size_t last, std::uint64_t seed)
    {
        Sums out;
        const double dt = T / n_steps, sqdt = std::sqrt(dt);
        const double disc = std::exp(-r * T);
        for (std::size_t p = first; p < last; ++p)
        {
            double S = S0, v = h.v0;
            for (int s = 0; s < n_steps; ++s)
            {
                const auto j = static_cast<std::uint32_t>(2 * s);
                const double z0 = inverse_normal_cdf(philox_uniform(seed, p, j));
                const double z1 = inverse_normal_cdf(philox_uniform(seed, p, j + 1));
                if (scheme == mc::VarianceScheme::QE)
                    mc::qe_step(h, 1.0, r, q, S, v, dt, z0, z1);
                else
                {
                    const double v_plus = std::max(v, mc::variance_floor());
                    const double sv = std::sqrt(v_plus);
                    S = S * std::exp(mc::heston_log_return(r, q, 0.0, 0.0, v_plus, sv, sqdt, dt, z0));
                    mc::heston_variance_step(h, v, v_plus, sv, sqdt, dt, z0, z1);
                }
            }
            for (int o = 0; o < 2; ++o)
            {
                const double x = disc * std::max(opts[o].call ? S - opts[o].strike : opts[o].strike - S, 0.0);
                out.s[o] += x;
                out.s2[o] += x * x;
            }
        }
        return out;
    }
} // namespace

int main(int argc, char **argv)
{
    const std::size_t paths = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : (1u << 22);
    const unsigned threads = argc > 2 ? std::atoi(argv[2]) : 16;
    const double S0 = 100.0, r = 0.04, q = 0.012, T = 1.0;
    const mc::HestonParamsT<double> spy{0.04851034923442465, 3.314161450366533, 0.03738503889780653,
                                        1.3771724076865512, -0.7015824182317305};
    const double fellers[] = {1.5, 0.5, 0.18, 2.0 * spy.kappa * spy.theta / (spy.xi * spy.xi)};
    const Option opts[2] = {{"ATM call", 100.0, true}, {"90 put", 90.0, false}};
    const int steps[] = {2, 4, 8, 16, 32, 64, 128, 252};

    std::printf("paths %zu, threads %u, S0 %.0f, r %.3f, q %.3f, T %.0f\n", paths, threads, S0, r, q, T);
    for (double F : fellers)
    {
        mc::HestonParamsT<double> h = spy;
        h.xi = std::sqrt(2.0 * h.kappa * h.theta / F);
        HestonParams hp{h.v0, h.kappa, h.theta, h.xi, h.rho};
        double cos_px[2];
        for (int o = 0; o < 2; ++o)
            cos_px[o] = heston_cos_price(S0 * std::exp((r - q) * T), opts[o].strike, T, std::exp(-r * T), hp,
                                         opts[o].call);
        std::printf("\nFeller %.2f (xi %.4f): COS %s %.6f, %s %.6f\n", F, h.xi, opts[0].name, cos_px[0],
                    opts[1].name, cos_px[1]);
        std::printf("%6s | %-8s %10s %8s | %10s %8s | %8s\n", "steps", "scheme", "call bias%", "se%", "put bias%", "se%",
                    "ms");
        for (int n : steps)
            for (auto scheme : {mc::VarianceScheme::Euler, mc::VarianceScheme::QE})
            {
                const auto t0 = std::chrono::steady_clock::now();
                std::vector<Sums> parts(threads);
                std::vector<std::thread> pool;
                for (unsigned t = 0; t < threads; ++t)
                    pool.emplace_back([&, t]
                                      { parts[t] = run(h, r, q, S0, T, n, scheme, opts, paths * t / threads,
                                                       paths * (t + 1) / threads, 20261119); });
                for (auto &th : pool)
                    th.join();
                const double ms =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                double bias[2], se[2];
                for (int o = 0; o < 2; ++o)
                {
                    double s = 0, s2 = 0;
                    for (const Sums &x : parts)
                        s += x.s[o], s2 += x.s2[o];
                    const double mean = s / paths;
                    const double var = (s2 / paths - mean * mean) * paths / (paths - 1.0);
                    bias[o] = 100.0 * (mean - cos_px[o]) / cos_px[o];
                    se[o] = 100.0 * std::sqrt(var / paths) / cos_px[o];
                }
                std::printf("%6d | %-8s %+10.3f %8.3f | %+10.3f %8.3f | %8.0f\n", n,
                            scheme == mc::VarianceScheme::QE ? "QE" : "Euler", bias[0], se[0], bias[1], se[1], ms);
            }
    }
    return 0;
}
