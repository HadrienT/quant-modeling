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
    /// lookup, the same bracketing as models/volatility.hpp's
    /// GridLocalVol::value(), generalised so the corners can be Number.
    /// The cell is chosen on the double value of the spot, but the strike
    /// weight is a function of the spot itself (T): sigma_loc(S_t) moves
    /// with S_t, and S_t with every earlier vol, so dropping that
    /// dependence would lose the part of a local vega (and of the delta)
    /// that flows through the path -- measured at ~19 % of the vega on a
    /// skewed SPY surface before this was fixed. Outside the grid the vol
    /// is flat in S, so the weight is then a constant.
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

    /// SLV leverage (SLVSimModel), floored at 1e-4.
    /// Bilinear in K, but *staircase* in T -- not the smooth bilinear
    /// bracketing mc::local_vol_bilinear() uses. This has to
    /// match market/slv_calibration.hpp's own convention exactly, not
    /// just look plausible: column j there was calibrated by stepping
    /// particles *from* column j-1's leverage held constant across the
    /// whole interval (T_grid[j-1], T_grid[j]], and reading off the
    /// resulting distribution at T_grid[j] -- so column j-1, not a
    /// blend of j-1 and j, is what is actually valid on that interval.
    /// Smoothly blending toward column j partway through the interval
    /// was tried first and measurably biased option prices (caught by
    /// SLVSimModel.CalibratedToFlatVolReproducesBlackScholes, off by
    /// ~6% at one strike -- more than Monte Carlo noise could explain).
    /// t <= T_grid.front() uses column 0 itself: the calibration's
    /// bootstrap step uses a single scalar leverage (every particle
    /// starts exactly at s0, so there is no S-dependence to speak of
    /// yet), which column 0 already approximates closely for a short
    /// first interval, and is not otherwise stored anywhere to look up.
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
    /// Returns the step's variance of the log-spot, σ² dt, for the barrier
    /// monitoring of bridge_survival() (unused, it costs nothing).
    template <class T>
    QM_HOST_DEVICE T local_vol_step(const GridView<T> &sigma_loc, const T &r, const T &q, T &S, double t,
                                    double dt, double z)
    {
        using std::exp;
        using std::sqrt;
        const T sig = local_vol_bilinear(sigma_loc, S, t);
        const T drift = (r - q - 0.5 * sig * sig) * dt;
        const T vol_sqrt_dt = sig * sqrt(dt);
        S *= exp(drift + vol_sqrt_dt * z);
        return vol_sqrt_dt * vol_sqrt_dt;
    }

    /**
     * @brief Probability that a log-Brownian bridge from `a` to `b` with
     *        log-variance `var` stays above (`above`) or below `level` --
     *        the continuous-monitoring correction of a barrier between two
     *        simulated dates (WP 26 P2).
     *
     * P(cross | a, b) = exp(-2 ln(L/a) ln(L/b) / var) when both endpoints
     * are on the safe side (Andersen & Brotherton-Ratcliffe; Glasserman
     * 2004, §6.4); an endpoint on or beyond the level is a crossing. Exact
     * for Black-Scholes; under local vol the step's own volatility makes it
     * the usual approximation. Same formula and same edge cases as the
     * conditional MC of engines/mc/barrier.cpp.
     */
    template <class T>
    QM_HOST_DEVICE T bridge_survival(const T &a, const T &b, double level, bool above, const T &var)
    {
        using std::exp;
        using std::log;
        if (above ? (a <= level || b <= level) : (a >= level || b >= level))
            return T(0.0);
        const T exponent = -2.0 * log(level / a) * log(level / b) / var;
        if (!(exponent < 0.0))
            return T(0.0); // var = 0 with both endpoints safe cannot happen on a drawn step
        return 1.0 - exp(exponent);
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
