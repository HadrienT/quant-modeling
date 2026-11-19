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

        /// The slope of the strike interpolation at node i as a combination
        /// of the node values, m(i) = a y(i-1) + b y(i) + c y(i+1): the
        /// weighted three-point difference inside, one-sided at the two ends.
        QM_HOST_DEVICE void node_slope(int i, double &a, double &b, double &c) const
        {
            if (i == 0)
            {
                const double hr = K[1] - K[0];
                a = 0.0, b = -1.0 / hr, c = 1.0 / hr;
            }
            else if (i == nK - 1)
            {
                const double hl = K[i] - K[i - 1];
                a = -1.0 / hl, b = 1.0 / hl, c = 0.0;
            }
            else
            {
                const double hl = K[i] - K[i - 1], hr = K[i + 1] - K[i];
                a = -hr / (hl * (hl + hr));
                c = hl / (hr * (hl + hr));
                b = -(a + c);
            }
        }

        /**
         * The local vol's strike interpolation since 2026-11-16 (maintainer's
         * decision, WP 27 §14.15): a cubic Hermite through the nodes with
         * finite-difference slopes (the weighted three-point one inside --
         * exact for a quadratic -- one-sided at the two ends), so the
         * surface is C¹ in the spot, which a pathwise second derivative
         * needs, and it is a linear function of the node values, so an
         * adjoint is still a share per node. In the cell (i0, i1) the value
         * is sum_k w[k] value(idx[k], j): four nodes, i0 - 1 .. i1 + 1
         * (clamped at the ends, where the outer weight is 0). The weights
         * are functions of the spot itself (T) inside the grid and constants
         * outside, where the surface is flat in S; dw[k] = d w[k] / d S
         * (0 outside). A two-node grid, or values linear in the strike, give
         * the linear interpolation.
         */
        QM_HOST_DEVICE void strike_hermite(const T &S_t, int idx[4], T w[4], T dw[4]) const
        {
            const double S_raw = value_of(S_t);
            const double Sc = std::clamp(S_raw, K[0], K[nK - 1]);
            const bool inside = Sc == S_raw;
            int i0, i1;
            bracket(K, nK, Sc, i0, i1);
            const double h = K[i1] - K[i0];
            idx[0] = i0 > 0 ? i0 - 1 : i0;
            idx[1] = i0;
            idx[2] = i1;
            idx[3] = i1 + 1 < nK ? i1 + 1 : i1;
            if (h <= 1e-12)
            {
                w[0] = w[2] = w[3] = T(0.0), w[1] = T(1.0);
                dw[0] = dw[1] = dw[2] = dw[3] = T(0.0);
                return;
            }
            double a0, b0, c0, a1, b1, c1;
            node_slope(i0, a0, b0, c0);
            node_slope(i1, a1, b1, c1);
            const T u = inside ? T((S_t - K[i0]) / h) : T((Sc - K[i0]) / h);
            const T u2 = u * u, u3 = u2 * u;
            const T h00 = 2.0 * u3 - 3.0 * u2 + 1.0, h10 = u3 - 2.0 * u2 + u;
            const T h01 = -2.0 * u3 + 3.0 * u2, h11 = u3 - u2;
            // y(i0 - 1) enters through m(i0) only, y(i1 + 1) through m(i1).
            w[0] = h * h10 * a0;
            w[1] = h00 + h * h10 * b0 + h * h11 * a1;
            w[2] = h01 + h * h10 * c0 + h * h11 * b1;
            w[3] = h * h11 * c1;
            if (!inside)
            {
                dw[0] = dw[1] = dw[2] = dw[3] = T(0.0);
                return;
            }
            const T d00 = (6.0 * u2 - 6.0 * u) / h, d10 = (3.0 * u2 - 4.0 * u + 1.0) / h;
            const T d01 = (-6.0 * u2 + 6.0 * u) / h, d11 = (3.0 * u2 - 2.0 * u) / h;
            dw[0] = h * d10 * a0;
            dw[1] = d00 + h * d10 * b0 + h * d11 * a1;
            dw[2] = d01 + h * d10 * c0 + h * d11 * b1;
            dw[3] = h * d11 * c1;
        }

        /// The second derivatives of strike_hermite's weights in the spot,
        /// d2w[k] = d² w[k] / d S², on the same four nodes: what the exact
        /// second order through the grid needs (WP 27 §14.16). 0 outside the
        /// strikes, where the surface is flat. They jump at the nodes (the
        /// interpolation is C¹, not C²), which a pathwise second derivative
        /// allows.
        QM_HOST_DEVICE void strike_hermite_curvature(double S, int idx[4], double d2w[4]) const
        {
            const double Sc = std::clamp(S, K[0], K[nK - 1]);
            int i0, i1;
            bracket(K, nK, Sc, i0, i1);
            const double h = K[i1] - K[i0];
            idx[0] = i0 > 0 ? i0 - 1 : i0;
            idx[1] = i0;
            idx[2] = i1;
            idx[3] = i1 + 1 < nK ? i1 + 1 : i1;
            d2w[0] = d2w[1] = d2w[2] = d2w[3] = 0.0;
            if (h <= 1e-12 || Sc != S)
                return;
            double a0, b0, c0, a1, b1, c1;
            node_slope(i0, a0, b0, c0);
            node_slope(i1, a1, b1, c1);
            const double u = (S - K[i0]) / h, h2 = h * h;
            const double e00 = (12.0 * u - 6.0) / h2, e10 = (6.0 * u - 4.0) / h2;
            const double e01 = (-12.0 * u + 6.0) / h2, e11 = (6.0 * u - 2.0) / h2;
            d2w[0] = h * e10 * a0;
            d2w[1] = e00 + h * e10 * b0 + h * e11 * a1;
            d2w[2] = e01 + h * e10 * c0 + h * e11 * b1;
            d2w[3] = h * e11 * c1;
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

    /// Local vol: C¹ in the strike (GridView::strike_hermite), linear in
    /// time, floored at 1e-6 -- LocalVolSimModel's lookup, the same as
    /// models/volatility.hpp's GridLocalVol::value(), generalised so the
    /// nodes can be Number. (Bilinear until 2026-11-16: its slope in the spot
    /// jumped at every strike, which a pathwise second derivative cannot
    /// see.) The cell is chosen on the double value of the spot, but the
    /// strike weights are functions of the spot itself (T): sigma_loc(S_t)
    /// moves with S_t, and S_t with every earlier vol, so dropping that
    /// dependence would lose the part of a local vega (and of the delta)
    /// that flows through the path -- measured at ~19 % of the vega on a
    /// skewed SPY surface before this was fixed. Outside the grid the vol is
    /// flat in S, so the weights are then constants.
    template <class T>
    QM_HOST_DEVICE T local_vol_bilinear(const GridView<T> &g, const T &S_t, double t)
    {
        using std::max;
        int idx[4], j0, j1;
        T w[4], dw[4];
        g.strike_hermite(S_t, idx, w, dw);
        t = std::clamp(t, g.T_grid[0], g.T_grid[g.nT - 1]);
        bracket(g.T_grid, g.nT, t, j0, j1);
        const double T0 = g.T_grid[j0];
        const double dT = g.T_grid[j1] - T0;
        const double wT = (dT > 1e-12) ? (t - T0) / dT : 0.0;
        T sigma = T(0.0);
        for (int k = 0; k < 4; ++k)
            sigma = sigma + w[k] * ((1.0 - wT) * g.at(idx[k], j0) + wT * g.at(idx[k], j1));
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

    /// The local vol and its slope in the spot, ∂σ/∂S, of the lookup above:
    /// 0 outside the strike grid (flat there) and where the floor binds.
    template <class T>
    QM_HOST_DEVICE T local_vol_with_slope(const GridView<T> &g, const T &S_t, double t, T &dsigma_dS)
    {
        int idx[4], j0, j1;
        T w[4], dw[4];
        g.strike_hermite(S_t, idx, w, dw);
        t = std::clamp(t, g.T_grid[0], g.T_grid[g.nT - 1]);
        bracket(g.T_grid, g.nT, t, j0, j1);
        const double T0 = g.T_grid[j0];
        const double dT = g.T_grid[j1] - T0;
        const double wT = (dT > 1e-12) ? (t - T0) / dT : 0.0;
        T sigma = T(0.0), slope = T(0.0);
        for (int k = 0; k < 4; ++k)
        {
            const T node = (1.0 - wT) * g.at(idx[k], j0) + wT * g.at(idx[k], j1);
            sigma = sigma + w[k] * node;
            slope = slope + dw[k] * node;
        }
        dsigma_dS = value_of(sigma) <= 1e-6 ? T(0.0) : slope;
        return value_of(sigma) > 1e-6 ? sigma : T(1e-6);
    }

    /// Milstein step of the log-spot X = ln S under local vol (WP 26 P4b):
    /// X += (r - q - σ²/2) dt + σ √dt z + ½ σ (S ∂σ/∂S) dt (z² - 1), the
    /// diffusion b(X) = σ(e^X, t) having b' = S ∂σ/∂S. Strong order 1 where
    /// σ is smooth (MLMC's beta = 2, Giles & Szpruch 2012), against 1/2 for
    /// the Euler step above. Returns σ² dt, as local_vol_step.
    template <class T>
    QM_HOST_DEVICE T local_vol_milstein_step(const GridView<T> &sigma_loc, const T &r, const T &q, T &S, double t,
                                             double dt, double z)
    {
        using std::exp;
        using std::sqrt;
        T slope;
        const T sig = local_vol_with_slope(sigma_loc, S, t, slope);
        const T drift = (r - q - 0.5 * sig * sig) * dt;
        const T vol_sqrt_dt = sig * sqrt(dt);
        const T correction = 0.5 * sig * (S * slope) * dt * (z * z - 1.0);
        S *= exp(drift + vol_sqrt_dt * z + correction);
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

    /// How the variance of Heston and SLV is stepped in a price run (#317).
    /// The adjoint and tangent risks always step it by Euler: QE's switch
    /// between its two branches and its atom at 0 make its pathwise
    /// derivatives discontinuous.
    enum class VarianceScheme : int
    {
        Euler = 0, ///< full truncation: heston_variance_step
        QE = 1     ///< Andersen's quadratic-exponential, martingale-corrected: qe_step
    };

    /// Andersen's switching level between the quadratic and the exponential
    /// branch (any value in [1, 2] works; 1.5 is his choice).
    QM_HOST_DEVICE constexpr double qe_psi_c()
    {
        return 1.5;
    }

    /**
     * @brief One step of Heston (L = 1) or SLV (L the leverage at the step's
     *        start) by Andersen's QE-M scheme: L. Andersen, "Simple and
     *        efficient simulation of the Heston stochastic volatility model",
     *        Journal of Computational Finance 11(3), 2008, §3.2.4 and §4.
     *
     * The variance V' is drawn from a law that matches the exact conditional
     * mean and variance of the CIR process (a scaled noncentral chi-square):
     * a(b + z_v)² when ψ = s²/m² <= ψ_c, else 0 with probability p and an
     * exponential tail otherwise -- so V stays >= 0 without a floor, and the
     * scheme stays accurate where the Feller condition fails badly, the
     * variance spending much of its time near 0 (SPY's calibrated fit,
     * 2026-11-18: 2κθ/ξ² = 0.13), which is where full-truncation Euler is
     * most biased.
     *
     * The log-spot integrates ∫ L √V dW with L frozen over the step, the
     * correlated part read from the variance's own increment (Andersen's
     * eq. 33 with ρ → Lρ and the drift's ½V → ½L²V), central weights
     * γ₁ = γ₂ = ½:
     *   ln S' = ln S + (r - q)dt + K₀* + K₁ V + K₂ V' + √(K₃ V + K₄ V') z_s,
     * K₀* being the martingale correction (Andersen's Proposition 9) that
     * makes E[S' | S, V] = S e^{(r-q)dt} exactly for the drawn law of V';
     * for SLV it uses L at the step's start, which Andersen's derivation
     * does not cover. Where the correction's condition fails (A at or
     * beyond the moment generating function's pole: positive Lρ and a long
     * step), the uncorrected K₀ is kept, as in engines/mc/heston_qe.cpp.
     *
     * Draws: z_v sets V' (the quadratic branch's normal, or U = Φ(z_v) in the
     * exponential one, its tail 1 - U = Φ(-z_v) computed directly), z_s the
     * spot's part independent of V. Plain doubles only: price runs.
     */
    QM_HOST_DEVICE inline void qe_step(const HestonParamsT<double> &h, double L, double r, double q, double &S,
                                       double &v, double dt, double z_v, double z_s)
    {
        using std::erfc;
        using std::exp;
        using std::expm1;
        using std::log;
        using std::sqrt;
        const double k = h.kappa, th = h.theta, xi = h.xi, rho = h.rho;
        const double ekt = exp(-k * dt);
        const double one_m = -expm1(-k * dt); // 1 - e^{-k dt}, exact for a small k dt
        const double m = th + (v - th) * ekt;
        const double s2 = v * xi * xi * ekt * one_m / k + th * xi * xi * one_m * one_m / (2.0 * k);
        const double psi = s2 / (m * m);

        const double Lr = L * rho;
        const double K0 = -Lr * k * th * dt / xi;
        const double c = 0.5 * dt * (k * Lr / xi - 0.5 * L * L);
        const double K1 = c - Lr / xi;
        const double K2 = c + Lr / xi;
        const double K3 = 0.5 * dt * L * L * (1.0 - rho * rho);
        const double K4 = K3;
        const double A = K2 + 0.5 * K4;

        double v_next, log_mgf = 0.0;
        bool corrected;
        if (psi <= qe_psi_c())
        {
            const double inv = 1.0 / psi;
            const double b2 = 2.0 * inv - 1.0 + sqrt(2.0 * inv) * sqrt(2.0 * inv - 1.0);
            const double a = m / (1.0 + b2);
            const double b = sqrt(b2);
            v_next = a * (b + z_v) * (b + z_v);
            corrected = A < 1.0 / (2.0 * a);
            if (corrected)
            {
                const double one_m2Aa = 1.0 - 2.0 * A * a;
                log_mgf = A * b2 * a / one_m2Aa - 0.5 * log(one_m2Aa);
            }
        }
        else
        {
            const double p = (psi - 1.0) / (psi + 1.0);
            const double beta = (1.0 - p) / m;                            // 2 / (m (psi + 1))
            const double tail = 0.5 * erfc(z_v * 0.70710678118654752440); // 1 - U = Phi(-z_v)
            v_next = (1.0 - tail <= p) ? 0.0 : log((1.0 - p) / tail) / beta;
            corrected = A < beta;
            if (corrected) // E[e^{A V'}] = p + beta (1 - p) / (beta - A): the atom at 0 counts
                log_mgf = log(p + beta * (1.0 - p) / (beta - A));
        }
        const double K0s = corrected ? -log_mgf - (K1 + 0.5 * K3) * v : K0;
        const double var = K3 * v + K4 * v_next;
        S = S * exp((r - q) * dt + K0s + K1 * v + K2 * v_next + sqrt(var > 0.0 ? var : 0.0) * z_s);
        v = v_next;
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
