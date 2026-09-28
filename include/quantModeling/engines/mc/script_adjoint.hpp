#ifndef QM_ENGINES_MC_SCRIPT_ADJOINT_HPP
#define QM_ENGINES_MC_SCRIPT_ADJOINT_HPP

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "quantModeling/core/platform.hpp"
#include "quantModeling/core/types.hpp"
#include "quantModeling/engines/mc/path_draws.hpp"
#include "quantModeling/engines/mc/script_path.hpp"
#include "quantModeling/scripting/bytecode_adjoint.hpp"
#include "quantModeling/utils/dual.hpp"

/**
 * @file script_adjoint.hpp
 * @brief A script's model risks path by path, without a tape -- host and
 *        device (blueprint/wp/19-gpu.md §6, lot G3, ADR-G4).
 *
 * Two mechanisms, by number of parameters:
 *
 * 1. Forward mode, for Black-Scholes and Heston (4 to 8 parameters):
 *    script_path<Dual<N>> with each parameter seeded on its own direction
 *    (script_dual_inputs). Directions follow the CPU model's
 *    parameter_labels(), so a dual run's risks line up with simulate_aad's.
 *
 * 2. Per-path adjoint, for local vol (spot, rate, div and every point of the
 *    sigma_loc grid -- 1 500 for 50 x 30): script_lv_adjoint_path runs the
 *    path forward, pushing each step's starting spot and each event's
 *    bytecode record on the path's trail, then back: the event's
 *    reverse_event gives d payoff / d spot, and local_vol_step_adjoint --
 *    the step's adjoint written by hand -- carries it back one step, adding
 *    the grid's share into the four corners of the step's cell. The draws
 *    are not stored: Philox regenerates them from (seed, path, step).
 *
 * The CPU tape (simulate_aad on the same models) is the oracle: same draws,
 * same risks up to rounding.
 */

namespace quantModeling::mc
{

    // ── 1. forward mode ────────────────────────────────────────────────────

    /// Parameters a dual run differentiates, in the CPU model's label order
    /// (0 when the model has no dual path on the device):
    ///   Black-Scholes, one asset:  spot, rate, div, vol
    ///   Black-Scholes, n assets:   rate, spot[i], div[i], vol[i]
    ///   Heston (BatesSimModel):    spot, rate, div, v0, kappa, theta, xi, rho
    ///                              (then the three jump labels, whose risks
    ///                              are zero with no jumps)
    QM_HOST_DEVICE inline int dual_directions(DeviceModel::Kind kind, int n_assets)
    {
        switch (kind)
        {
            case DeviceModel::Kind::BlackScholes:
                return n_assets == 1 ? 4 : 1 + 3 * n_assets;
            case DeviceModel::Kind::Heston:
                return 8;
            default:
                return 0;
        }
    }

    template <int N>
    QM_HOST_DEVICE ScriptModelInputs<Dual<N>> script_dual_inputs(const ScriptPathView &v)
    {
        using D = Dual<N>;
        ScriptModelInputs<D> in = script_inputs<D>(v);
        auto seed = [](D &x, int i)
        {
            if (i < N)
                x.d[i] = 1.0;
        };
        const int n = v.n_assets;
        if (v.kind == DeviceModel::Kind::BlackScholes && n > 1)
        {
            seed(in.r, 0);
            for (int k = 0; k < n; ++k)
            {
                seed(in.s0[k], 1 + k);
                seed(in.divs[k], 1 + n + k);
                seed(in.vols[k], 1 + 2 * n + k);
            }
            return in;
        }
        seed(in.s0[0], 0);
        seed(in.r, 1);
        if (v.kind == DeviceModel::Kind::BlackScholes)
        {
            seed(in.divs[0], 2);
            seed(in.vols[0], 3);
            return in;
        }
        seed(in.q, 2);
        seed(in.heston.v0, 3);
        seed(in.heston.kappa, 4);
        seed(in.heston.theta, 5);
        seed(in.heston.xi, 6);
        seed(in.heston.rho, 7);
        return in;
    }

    // ── 2. per-path adjoint, local vol ─────────────────────────────────────

    /// A gradient row: parameter p at base[p * stride] (stride = the GPU
    /// threads sharing the buffer; 1 on the host). Local-vol order: spot,
    /// rate, div, then sigma_loc K-major -- LocalVolSimModel's labels.
    struct GradientRow
    {
        double *base = nullptr;
        long stride = 1;
        QM_HOST_DEVICE double &operator[](long p) const { return base[p * stride]; }
    };

    /// A sigma_loc adjoint grid inside a gradient row (after spot, rate, div).
    struct GridRow
    {
        GradientRow row;
        int nT = 0;
        QM_HOST_DEVICE void add(int i, int j, double x) const { row[3 + static_cast<long>(i) * nT + j] += x; }
    };

    /// Trail entries one local-vol path needs: a spot per step, and each
    /// event's bytecode record.
    inline long lv_trail_bound(const scripting::Program &p, int n_steps)
    {
        long n = n_steps;
        for (std::size_t e = 0; e < p.n_events(); ++e)
            n += scripting::trail_bound(p, e);
        return n;
    }

    /**
     * @brief Adjoint of local_vol_step (path_steps.hpp) from the step's
     *        starting spot S: given aS = d payoff / d S_after, add the
     *        rate, dividend and grid shares, and return d payoff / d S.
     *
     * Forward: sigma = max(bilinear(S, t), 1e-6), x = (r - q - sigma^2/2) dt
     * + sigma sqrt(dt) z, S_after = S e^x. The strike weight is a function
     * of S inside the grid (not at its flat edges), which is how the spot's
     * adjoint picks up the vol's dependence on the path.
     */
    QM_HOST_DEVICE inline double local_vol_step_adjoint(const GridView<Real> &g, Real r, Real q, double S, double t,
                                                        double dt, double z, double aS, double &a_r, double &a_q,
                                                        const GridRow &grid_adj)
    {
        // forward, as local_vol_bilinear and local_vol_step compute it
        int i0, i1, j0, j1;
        const double S_raw = S;
        const double Sc = S_raw < g.K[0] ? g.K[0] : (S_raw > g.K[g.nK - 1] ? g.K[g.nK - 1] : S_raw);
        const bool inside = Sc == S_raw;
        bracket(g.K, g.nK, Sc, i0, i1);
        const double K0 = g.K[i0];
        const double dK = g.K[i1] - K0;
        const double wK = dK <= 1e-12 ? 0.0 : (inside ? (S - K0) / dK : (Sc - K0) / dK);
        double tc = t < g.T_grid[0] ? g.T_grid[0] : (t > g.T_grid[g.nT - 1] ? g.T_grid[g.nT - 1] : t);
        bracket(g.T_grid, g.nT, tc, j0, j1);
        const double T0 = g.T_grid[j0];
        const double dT = g.T_grid[j1] - T0;
        const double wT = (dT > 1e-12) ? (tc - T0) / dT : 0.0;
        const double g00 = g.at(i0, j0), g10 = g.at(i1, j0), g01 = g.at(i0, j1), g11 = g.at(i1, j1);
        const double sigma_b = (1.0 - wK) * (1.0 - wT) * g00 + wK * (1.0 - wT) * g10 + (1.0 - wK) * wT * g01 +
                               wK * wT * g11;
        const double sig = sigma_b > 1e-6 ? sigma_b : 1e-6;
        const double sqdt = std::sqrt(dt);
        const double drift = (r - q - 0.5 * sig * sig) * dt;
        const double E = std::exp(drift + sig * sqdt * z);

        // back
        const double a_S_prev = aS * E;
        const double a_x = aS * S * E;
        const double a_inner = a_x * dt; // d/d(r - q - sigma^2/2)
        a_r += a_inner;
        a_q -= a_inner;
        const double a_sig = a_x * z * sqdt - a_inner * sig;
        double a_S = a_S_prev;
        if (sigma_b > 1e-6)
        {
            const double a_sb = a_sig;
            grid_adj.add(i0, j0, a_sb * ((1.0 - wK) * (1.0 - wT)));
            grid_adj.add(i1, j0, a_sb * (wK * (1.0 - wT)));
            grid_adj.add(i0, j1, a_sb * ((1.0 - wK) * wT));
            grid_adj.add(i1, j1, a_sb * (wK * wT));
            if (inside && dK > 1e-12)
            {
                const double a_wK = a_sb * ((1.0 - wT) * (g10 - g00) + wT * (g11 - g01));
                a_S += a_wK / dK;
            }
        }
        return a_S;
    }

    /// Per-path adjoint state, sized by the script (ScriptLimits on the device).
    struct AdjointScratch
    {
        scripting::Trail trail;
        GradientRow grad;
    };

    /**
     * @brief One local-vol path of a script, forward then back: its payoff,
     *        and `weight` x d payoff / d (spot, rate, div, sigma_loc) added
     *        into `grad`. Plain Philox draws (unit u), no antithetic mirror.
     */
    QM_HOST_DEVICE inline double script_lv_adjoint_path(const ScriptPathView &v, const PathDraws &draws_cfg,
                                                        uint64_t u, double weight, AdjointScratch &w)
    {
        using L = ScriptLimits;
        double vars[L::kVars], stack[L::kStack], degrees[L::kDegrees], slots[L::kIfSlots];
        int modes[L::kIfModes];
        double disc[L::kDiscounts];

        PathDraws draws = draws_cfg;
        draws.begin(u);
        scripting::Trail &trail = w.trail;
        trail.top = 0;

        // ── forward ──
        for (int i = 0; i < v.n_vars; ++i)
            vars[i] = v.baseline ? v.baseline[i] : 0.0;
        scripting::Machine<double> m{vars, stack, degrees, slots, modes, 0.0};
        double S = v.s0[0];
        Time t_prev = 0.0;
        for (int s = 0; s < v.n_steps; ++s)
        {
            const Time ts = v.t[s];
            trail.push(S);
            local_vol_step(v.grid, v.r, v.q, S, ts, ts - t_prev, draws(s, 0));
            t_prev = ts;
            const int e = v.event[s];
            if (e < 0)
                continue;
            int nd = 0;
            for (int i = v.disc_begin[e]; i < v.disc_begin[e + 1]; ++i)
                disc[nd++] = std::exp(-v.r * (v.disc_mats[i] - ts));
            const double numeraire = std::exp(v.r * ts);
            scripting::run_event_recorded(v.prog, v.event_begin[e], v.event_begin[e + 1], m, &S, disc, numeraire,
                                          trail);
        }
        const double payoff = m.payoff;

        // ── back ──
        double a_vars[L::kVars], a_stack[L::kStack], a_degrees[L::kDegrees], a_slots[L::kIfSlots];
        for (int i = 0; i < v.n_vars; ++i)
            a_vars[i] = 0.0;
        for (int i = 0; i < L::kIfSlots; ++i)
            a_slots[i] = 0.0;
        scripting::AdjointMachine a{a_vars, a_stack, a_degrees, a_slots, weight};
        const GridRow grid_adj{w.grad, v.grid.nT};
        double aS = 0.0, a_r = 0.0, a_q = 0.0;
        for (int s = v.n_steps - 1; s >= 0; --s)
        {
            const Time ts = v.t[s];
            const int e = v.event[s];
            if (e >= 0)
            {
                double a_spot = 0.0, a_num = 0.0;
                double a_disc[L::kDiscounts];
                const int n_disc = v.disc_begin[e + 1] - v.disc_begin[e];
                for (int i = 0; i < n_disc; ++i)
                    a_disc[i] = 0.0;
                scripting::reverse_event(v.prog, a, &a_spot, a_disc, a_num, trail);
                aS += a_spot;
                for (int i = 0; i < n_disc; ++i)
                {
                    const double tau = v.disc_mats[v.disc_begin[e] + i] - ts;
                    a_r += a_disc[i] * std::exp(-v.r * tau) * -tau;
                }
                a_r += a_num * std::exp(v.r * ts) * ts;
            }
            const double S_prev = trail.pop();
            const Time t0 = s > 0 ? v.t[s - 1] : 0.0;
            aS = local_vol_step_adjoint(v.grid, v.r, v.q, S_prev, ts, ts - t0, draws.plain(static_cast<uint32_t>(s)), aS,
                                        a_r, a_q, grid_adj);
        }
        w.grad[0] += aS;
        w.grad[1] += a_r;
        w.grad[2] += a_q;
        return payoff;
    }

} // namespace quantModeling::mc

#endif // QM_ENGINES_MC_SCRIPT_ADJOINT_HPP
