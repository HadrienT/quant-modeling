#ifndef QM_BENCHMARKS_REFERENCE_PATHS_HPP
#define QM_BENCHMARKS_REFERENCE_PATHS_HPP

// Hand-written reference paths (blueprint/wp/26-performance.md, lot P0): the
// ceiling the generic script engine is measured against. No genericity at
// all: one product, one model, a straight loop that keeps only its running
// state. Never part of the library; the CPU and GPU benchmarks share these
// functions.
//
// Same products as benchmarks/perf_matrix.py (52 weekly dates, 252 equal
// Asian fixings, the same local-vol grid), the same estimator as the engines
// it is compared with: Philox gaussians, antithetic pairs, one unit = one
// pair.

#include "quantModeling/core/platform.hpp"
#include "quantModeling/utils/inverse_normal.hpp"
#include "quantModeling/utils/philox.hpp"

#include <cmath>
#include <cstdint>

namespace qm_reference
{

    /// sigma_loc(K, T) on a row-major K x T grid, bilinear inside, flat outside.
    struct LocalVolGrid
    {
        const double *K = nullptr;
        int nK = 0;
        const double *T = nullptr;
        int nT = 0;
        const double *sigma = nullptr;
    };

    QM_HOST_DEVICE inline int bracket(const double *x, int n, double v)
    {
        int lo = 0, hi = n - 1;
        while (hi - lo > 1)
        {
            const int mid = (lo + hi) / 2;
            if (v < x[mid])
                hi = mid;
            else
                lo = mid;
        }
        return lo;
    }

    QM_HOST_DEVICE inline double local_vol(const LocalVolGrid &g, double S, double t)
    {
        const double k = fmin(fmax(S, g.K[0]), g.K[g.nK - 1]);
        const double u = fmin(fmax(t, g.T[0]), g.T[g.nT - 1]);
        const int i = bracket(g.K, g.nK, k), j = bracket(g.T, g.nT, u);
        const double a = (k - g.K[i]) / (g.K[i + 1] - g.K[i]);
        const double b = (u - g.T[j]) / (g.T[j + 1] - g.T[j]);
        const double *s = g.sigma;
        const int nT = g.nT;
        return (1 - a) * (1 - b) * s[i * nT + j] + a * (1 - b) * s[(i + 1) * nT + j] +
               (1 - a) * b * s[i * nT + j + 1] + a * b * s[(i + 1) * nT + j + 1];
    }

    struct Terms
    {
        double s0, r, q, sigma; ///< sigma: the flat vol when there is no grid
        double strike, barrier;
        double maturity;
        int steps;
        double discount; ///< exp(-r T)
    };

    /// One path of an up-and-out call monitored at the `steps` equal dates;
    /// `sign` = -1 gives the antithetic path.
    QM_HOST_DEVICE inline double up_and_out(const Terms &m, const LocalVolGrid *grid, uint64_t seed, uint64_t path,
                                            double sign)
    {
        quantModeling::PhiloxGaussianSource z(seed, path);
        const double dt = m.maturity / m.steps, sqdt = sqrt(dt);
        double S = m.s0;
        for (int i = 0; i < m.steps; ++i)
        {
            const double sig = grid != nullptr ? local_vol(*grid, S, i * dt) : m.sigma;
            S *= exp((m.r - m.q - 0.5 * sig * sig) * dt + sig * sqdt * sign * z.next());
            if (S >= m.barrier)
                return 0.0;
        }
        return m.discount * fmax(S - m.strike, 0.0);
    }

    /// One path of an arithmetic Asian call, fixings at the `steps` equal dates.
    QM_HOST_DEVICE inline double asian(const Terms &m, const LocalVolGrid *grid, uint64_t seed, uint64_t path,
                                       double sign)
    {
        quantModeling::PhiloxGaussianSource z(seed, path);
        const double dt = m.maturity / m.steps, sqdt = sqrt(dt);
        double S = m.s0, sum = 0.0;
        for (int i = 0; i < m.steps; ++i)
        {
            const double sig = grid != nullptr ? local_vol(*grid, S, i * dt) : m.sigma;
            S *= exp((m.r - m.q - 0.5 * sig * sig) * dt + sig * sqdt * sign * z.next());
            sum += S;
        }
        return m.discount * fmax(sum / m.steps - m.strike, 0.0);
    }

    /// One path of a worst-of Phoenix autocall on three assets of the same
    /// spot and vol, pairwise correlation 0.4, observed at the `steps` equal
    /// dates: redeemed with its coupon when the worst performance is at or
    /// above 100 %, a coupon when at or above 70 %, capital at risk below
    /// 60 % at maturity (the script library's worst-of-autocall). The value
    /// is the sum of the discounted payments.
    QM_HOST_DEVICE inline double worst_of_autocall(const Terms &m, uint64_t seed, uint64_t path, double sign)
    {
        const double notional = 1000.0, coupon = 0.025, autocall = 1.0, coupon_barrier = 0.7, protection = 0.6;
        const double rho = 0.4; // Cholesky of the constant-correlation 3 x 3
        const double l22 = sqrt(1.0 - rho * rho), l32 = (rho - rho * rho) / l22,
                     l33 = sqrt(1.0 - rho * rho - l32 * l32);
        quantModeling::PhiloxGaussianSource z(seed, path);
        const double dt = m.maturity / m.steps, drift = (m.r - m.q - 0.5 * m.sigma * m.sigma) * dt,
                     vol = m.sigma * sqrt(dt);
        double x0 = 0.0, x1 = 0.0, x2 = 0.0, pv = 0.0; // log performances
        for (int i = 0; i < m.steps; ++i)
        {
            const double z0 = sign * z.next(), z1 = sign * z.next(), z2 = sign * z.next();
            x0 += drift + vol * z0;
            x1 += drift + vol * (rho * z0 + l22 * z1);
            x2 += drift + vol * (rho * z0 + l32 * z1 + l33 * z2);
            const double worst = exp(fmin(x0, fmin(x1, x2)));
            const double df = exp(-m.r * (i + 1) * dt);
            if (i + 1 == m.steps)
            {
                if (worst >= coupon_barrier)
                    return pv + df * notional * (1.0 + coupon);
                return pv + df * notional * (worst < protection ? worst : 1.0);
            }
            if (worst >= autocall)
                return pv + df * notional * (1.0 + coupon);
            if (worst >= coupon_barrier)
                pv += df * notional * coupon;
        }
        return pv;
    }

    enum class Product
    {
        UpAndOut,
        Asian,
        WorstOfAutocall
    };

    /// One unit: the mean of a path and its antithetic.
    QM_HOST_DEVICE inline double unit(Product p, const Terms &m, const LocalVolGrid *grid, uint64_t seed,
                                      uint64_t u)
    {
        if (p == Product::UpAndOut)
            return 0.5 * (up_and_out(m, grid, seed, u, 1.0) + up_and_out(m, grid, seed, u, -1.0));
        if (p == Product::WorstOfAutocall)
            return 0.5 * (worst_of_autocall(m, seed, u, 1.0) + worst_of_autocall(m, seed, u, -1.0));
        return 0.5 * (asian(m, grid, seed, u, 1.0) + asian(m, grid, seed, u, -1.0));
    }

} // namespace qm_reference

#endif // QM_BENCHMARKS_REFERENCE_PATHS_HPP
