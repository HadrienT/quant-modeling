#ifndef QM_MODELS_EQUITY_PATH_STEPS_HPP
#define QM_MODELS_EQUITY_PATH_STEPS_HPP

#include <algorithm>
#include <cmath>

#include "quantModeling/core/platform.hpp"
#include "quantModeling/core/types.hpp"

/**
 * @file path_steps.hpp
 * @brief The models' time steps as flat functors, host and device
 *        (blueprint/wp/19-gpu.md §4, lot G1).
 *
 * One step of each diffusion, written once: the CPU simulation models
 * (LocalVolSimModel, SLVSimModel, BatesSimModel) call these, and the GPU
 * kernels will call the same code on plain arrays -- no virtual call, no
 * allocation, no container, only pointers to grids the caller owns.
 *
 * Templated on the number type T like the models (double, or aad::Number on
 * the CPU for adjoint risks); a GPU kernel only ever instantiates T = double.
 * The arithmetic is the models' own, operation for operation: moving it here
 * changed no price (the existing tests pin them).
 */

namespace quantModeling::mc
{

    /// A T's value as a double (the grid cell is chosen on the value; only
    /// the weights inside it are differentiated).
    QM_HOST_DEVICE inline double value_of(double x)
    {
        return x;
    }
    template <class T>
    double value_of(const T &x)
    {
        return x.value();
    }

    /// First index i with x[i] >= v in the sorted x[0..n) (n if none):
    /// std::lower_bound, written out so that it compiles for the device.
    QM_HOST_DEVICE inline int lower_index(const Real *x, int n, double v)
    {
        int lo = 0, hi = n;
        while (lo < hi)
        {
            const int mid = lo + (hi - lo) / 2;
            if (x[mid] < v)
                lo = mid + 1;
            else
                hi = mid;
        }
        return lo;
    }

    /// The bracketing pair (i0, i1) of v in the grid x[0..n), n >= 2, as the
    /// models always chose it: i1 = lower_index clamped to the last node,
    /// i0 its left neighbour (i1 bumped to 1 at the left edge).
    QM_HOST_DEVICE inline void bracket(const Real *x, int n, double v, int &i0, int &i1)
    {
        i1 = lower_index(x, n, v);
        if (i1 >= n)
            i1 = n - 1;
        i0 = (i1 > 0) ? i1 - 1 : 0;
        if (i0 == i1)
        {
            if (i1 > 0)
                i0 = i1 - 1;
            else
                i1 = 1;
        }
    }

    /// A strike x time grid of values, K-major: value(i, j) = v[i * nT + j].
    template <class T>
    struct GridView
    {
        const Real *K = nullptr;
        int nK = 0;
        const Real *T_grid = nullptr;
        int nT = 0;
        const T *v = nullptr;

        QM_HOST_DEVICE const T &at(int i, int j) const
        {
            return v[static_cast<long>(i) * nT + j];
        }

        /// Strike weight of S_t in its cell: a function of the spot itself
        /// (T) inside the grid, a constant outside where the surface is flat
        /// in S. Sets the cell (i0, i1).
        QM_HOST_DEVICE T strike_weight(const T &S_t, int &i0, int &i1) const
        {
            const double S_raw = value_of(S_t);
            const double S = std::clamp(S_raw, K[0], K[nK - 1]);
            const bool inside = S == S_raw;
            bracket(K, nK, S, i0, i1);
            const double K0 = K[i0];
            const double dK = K[i1] - K0;
            return dK <= 1e-12 ? T(0.0) : inside ? T((S_t - K0) / dK)
                                                 : T((S - K0) / dK);
        }
    };

    /// Local vol: bilinear in (K, T), floored at 1e-6 -- LocalVolSimModel's
    /// lookup (models/volatility.hpp's GridLocalVol bracketing).
    template <class T>
    QM_HOST_DEVICE T local_vol_bilinear(const GridView<T> &g, const T &S_t, double t)
    {
        using std::max;
        int i0, i1, j0, j1;
        const T wK = g.strike_weight(S_t, i0, i1);
        t = std::clamp(t, g.T_grid[0], g.T_grid[g.nT - 1]);
        bracket(g.T_grid, g.nT, t, j0, j1);
        const double T0 = g.T_grid[j0];
        const double dT = g.T_grid[j1] - T0;
        const double wT = (dT > 1e-12) ? (t - T0) / dT : 0.0;
        const T sigma = (1.0 - wK) * (1.0 - wT) * g.at(i0, j0) +
                        wK * (1.0 - wT) * g.at(i1, j0) +
                        (1.0 - wK) * wT * g.at(i0, j1) +
                        wK * wT * g.at(i1, j1);
        return max(sigma, 1e-6);
    }

    /// SLV leverage: bilinear in K, staircase in T (column j-1 on
    /// (T_{j-1}, T_j], the calibration's own convention -- see
    /// SLVSimModel::leverage_at), floored at 1e-4.
    template <class T>
    QM_HOST_DEVICE T leverage_staircase(const GridView<T> &g, const T &S_t, double t)
    {
        using std::max;
        int i0, i1;
        const T wK = g.strike_weight(S_t, i0, i1);
        const double tc = std::clamp(t, g.T_grid[0], g.T_grid[g.nT - 1]);
        const int j = lower_index(g.T_grid, g.nT, tc);
        const int jcol = (j > 0) ? j - 1 : 0;
        const T lev = (1.0 - wK) * g.at(i0, jcol) + wK * g.at(i1, jcol);
        return max(lev, 1e-4);
    }

    /// Euler step of the log-spot under local vol: S *= exp((r - q - σ²/2)dt + σ√dt z).
    template <class T>
    QM_HOST_DEVICE void local_vol_step(const GridView<T> &sigma_loc, const T &r, const T &q, T &S, double t,
                                       double dt, double z)
    {
        using std::exp;
        using std::sqrt;
        const T sig = local_vol_bilinear(sigma_loc, S, t);
        const T drift = (r - q - 0.5 * sig * sig) * dt;
        const T vol_sqrt_dt = sig * sqrt(dt);
        S *= exp(drift + vol_sqrt_dt * z);
    }

    /// Heston parameters (the diffusion part of Bates and of SLV).
    template <class T>
    struct HestonParamsT
    {
        T v0, kappa, theta, xi, rho;
    };

    /// Variance floor of the full-truncation scheme (aad::sqrt's adjoint is
    /// infinite at 0 -- see BatesSimModel). A function, not a variable: the
    /// device cannot take a host constant's address, which std::max does.
    QM_HOST_DEVICE constexpr double variance_floor()
    {
        return 1e-10;
    }

    /// Full-truncation Euler step of the variance, from the pre-step
    /// v_plus = max(v, floor) and √v_plus.
    template <class T>
    QM_HOST_DEVICE void heston_variance_step(const HestonParamsT<T> &h, T &v, const T &v_plus, const T &sqrt_v_plus,
                                             double sqdt, double dt, double z_spot, double z_indep)
    {
        using std::sqrt;
        const T dW_vol = h.rho * z_spot + sqrt(1.0 - h.rho * h.rho) * z_indep;
        v = v + h.kappa * (h.theta - v_plus) * dt + h.xi * sqrt_v_plus * (sqdt * dW_vol);
    }

    /// Heston log-return over one step, before jumps, with Bates's jump
    /// compensator λk (λ = 0 for Heston). λ and k come in separately so that
    /// the expression is recorded on an AAD tape in Bates's own order.
    template <class T>
    QM_HOST_DEVICE T heston_log_return(const T &r, const T &q, const T &lambda, const T &k, const T &v_plus,
                                       const T &sqrt_v_plus, double sqdt, double dt, double z_spot)
    {
        return (r - q - lambda * k - 0.5 * v_plus) * dt + sqrt_v_plus * (sqdt * z_spot);
    }

    /// One SLV step: Heston with the spot's vol scaled by the leverage L(S, t).
    template <class T>
    QM_HOST_DEVICE void slv_step(const GridView<T> &leverage, const HestonParamsT<T> &h, const T &r, const T &q,
                                 T &S, T &v, double t, double dt, double z_spot, double z_indep)
    {
        using std::exp;
        using std::max;
        using std::sqrt;
        const double sqdt = std::sqrt(dt);
        const T Lp = leverage_staircase(leverage, S, t);
        const T v_plus = max(v, variance_floor());
        const T sqrt_v_plus = sqrt(v_plus);
        const T log_return = (r - q - 0.5 * Lp * Lp * v_plus) * dt + Lp * sqrt_v_plus * (sqdt * z_spot);
        S = S * exp(log_return);
        heston_variance_step(h, v, v_plus, sqrt_v_plus, sqdt, dt, z_spot, z_indep);
    }

} // namespace quantModeling::mc

#endif // QM_MODELS_EQUITY_PATH_STEPS_HPP
