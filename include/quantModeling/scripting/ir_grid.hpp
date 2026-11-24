#ifndef QM_SCRIPTING_IR_GRID_HPP
#define QM_SCRIPTING_IR_GRID_HPP

#include "quantModeling/core/platform.hpp"
#include "quantModeling/core/types.hpp"
#include "quantModeling/models/equity/path_steps.hpp"

/**
 * @file ir_grid.hpp
 * @brief What the IR's grid operations compute, host and device
 *        (blueprint/wp/27-ir-ad.md §3.3): the local-vol surface and the
 *        SLV leverage are looked up by the models' own functions
 *        (path_steps.hpp); here are their slope in the spot and the share of
 *        each grid point, which the tangent and the adjoint need.
 *
 * Same conventions as the CPU tape on the models' lookups: the cell is
 * chosen on the spot's value, the strike weight is a function of the spot
 * inside the strikes and a constant outside, and nothing flows through a
 * value the floor replaced.
 */

namespace quantModeling::scripting::ir::detail
{

    /// The nodes and weights of the local vol at (S, t) -- four strikes
    /// (GridView::strike_hermite, C¹ in the spot) by two times -- and whether
    /// the floor leaves it free (mc::local_vol_bilinear's arithmetic).
    struct GridCell
    {
        int idx[4], j0, j1;
        double w[4], dw[4], wT;
        double sigma; ///< the interpolated value, before the floor
        bool inside, free;
    };

    /// `hint`, when given, starts the cell's searches from the path's last
    /// one (mc::lower_index_from): the same cell, found in fewer loads.
    QM_HOST_DEVICE inline GridCell grid_cell(const mc::GridView<Real> &g, double S, double t,
                                             mc::GridHint *hint = nullptr)
    {
        GridCell c{};
        c.inside = S >= g.K[0] && S <= g.K[g.nK - 1];
        g.strike_hermite(S, c.idx, c.w, c.dw, hint ? &hint->k : nullptr);
        const double tc = t < g.T_grid[0] ? g.T_grid[0] : (t > g.T_grid[g.nT - 1] ? g.T_grid[g.nT - 1] : t);
        mc::bracket(g.T_grid, g.nT, tc, c.j0, c.j1, hint ? &hint->t : nullptr);
        const double T0 = g.T_grid[c.j0];
        const double dT = g.T_grid[c.j1] - T0;
        c.wT = (dT > 1e-12) ? (tc - T0) / dT : 0.0;
        c.sigma = 0.0;
        for (int k = 0; k < 4; ++k)
            c.sigma = c.sigma + c.w[k] * ((1.0 - c.wT) * g.at(c.idx[k], c.j0) + c.wT * g.at(c.idx[k], c.j1));
        c.free = c.sigma > 1e-6;
        return c;
    }

    // The same from a cell already found: a backward step looks its cell up
    // once for the vol, its slope and the nodes' shares.
    QM_HOST_DEVICE inline double grid_slope(const mc::GridView<Real> &g, const GridCell &c)
    {
        if (!c.free || !c.inside)
            return 0.0;
        double slope = 0.0;
        for (int k = 0; k < 4; ++k)
            slope += c.dw[k] * ((1.0 - c.wT) * g.at(c.idx[k], c.j0) + c.wT * g.at(c.idx[k], c.j1));
        return slope;
    }

    /// `a` times each of the eight nodes' shares, handed to add(parameter,
    /// adjoint) in a fixed order: a dense row adds them in place, the GPU's
    /// adjoint appends them to the path's sparse trail (issue #342).
    template <class Add>
    QM_HOST_DEVICE inline void grid_accumulate_to(const GridCell &c, long nT, double a, int grid0, Add &&add)
    {
        if (!c.free)
            return;
        for (int k = 0; k < 4; ++k)
        {
            add(grid0 + c.idx[k] * nT + c.j0, a * (c.w[k] * (1.0 - c.wT)));
            add(grid0 + c.idx[k] * nT + c.j1, a * (c.w[k] * c.wT));
        }
    }

    QM_HOST_DEVICE inline void grid_accumulate(const GridCell &c, long nT, double a, double *base, long stride,
                                               int grid0)
    {
        grid_accumulate_to(c, nT, a, grid0, [&](long p, double v)
                           { base[p * stride] += v; });
    }

    /// `a` times each node's weight slope in the spot, added to the nodes'
    /// adjoints: the part of the tangent of grid_accumulate that comes from
    /// the spot moving (0 outside the strikes and where the floor binds).
    QM_HOST_DEVICE inline void grid_slope_accumulate(const GridCell &c, long nT, double a, double *base, long stride,
                                                     int grid0)
    {
        if (!c.free || !c.inside)
            return;
        for (int k = 0; k < 4; ++k)
        {
            base[(grid0 + c.idx[k] * nT + c.j0) * stride] += a * (c.dw[k] * (1.0 - c.wT));
            base[(grid0 + c.idx[k] * nT + c.j1) * stride] += a * (c.dw[k] * c.wT);
        }
    }

    /// d² sigma_loc / d S²: 0 outside the strikes and where the floor binds.
    /// The interpolation is C¹ in the spot inside the strikes, so this
    /// exists except at the nodes; the slope still jumps at the two end
    /// strikes (flat outside) and where the floor starts to bind.
    QM_HOST_DEVICE inline double grid_curvature(const mc::GridView<Real> &g, double S, double t)
    {
        const GridCell c = grid_cell(g, S, t);
        if (!c.free || !c.inside)
            return 0.0;
        int idx[4];
        double d2w[4];
        g.strike_hermite_curvature(S, idx, d2w);
        double curvature = 0.0;
        for (int k = 0; k < 4; ++k)
            curvature += d2w[k] * ((1.0 - c.wT) * g.at(idx[k], c.j0) + c.wT * g.at(idx[k], c.j1));
        return curvature;
    }

    /// d sigma_loc / d S: 0 outside the strikes and where the floor binds.
    QM_HOST_DEVICE inline double grid_slope(const mc::GridView<Real> &g, double S, double t)
    {
        return grid_slope(g, grid_cell(g, S, t));
    }

    /// `a` times each node's weight, added to the nodes' adjoints
    /// (parameter p at base[p * stride]).
    QM_HOST_DEVICE inline void grid_accumulate(const mc::GridView<Real> &g, double S, double t, double a,
                                               double *base, long stride, int grid0)
    {
        grid_accumulate(grid_cell(g, S, t), g.nT, a, base, stride, grid0);
    }

    /// The two corners of the SLV leverage at (S, t): linear in the strike,
    /// a staircase in time (mc::leverage_staircase).
    struct LeverageCell
    {
        int i0, i1, j;
        double wK, dK;
        bool inside, free;
    };

    QM_HOST_DEVICE inline LeverageCell leverage_cell(const mc::GridView<Real> &g, double S, double t,
                                                     mc::GridHint *hint = nullptr)
    {
        LeverageCell c{};
        const double Sc = S < g.K[0] ? g.K[0] : (S > g.K[g.nK - 1] ? g.K[g.nK - 1] : S);
        c.inside = Sc == S;
        mc::bracket(g.K, g.nK, Sc, c.i0, c.i1, hint ? &hint->k : nullptr);
        const double K0 = g.K[c.i0];
        c.dK = g.K[c.i1] - K0;
        c.wK = c.dK <= 1e-12 ? 0.0 : (c.inside ? (S - K0) / c.dK : (Sc - K0) / c.dK);
        const double tc = t < g.T_grid[0] ? g.T_grid[0] : (t > g.T_grid[g.nT - 1] ? g.T_grid[g.nT - 1] : t);
        const int j = hint ? mc::lower_index_from(g.T_grid, g.nT, tc, hint->t) : mc::lower_index(g.T_grid, g.nT, tc);
        c.j = (j > 0) ? j - 1 : 0;
        c.free = (1.0 - c.wK) * g.at(c.i0, c.j) + c.wK * g.at(c.i1, c.j) > 1e-4;
        return c;
    }

    // The same from a cell already found (a backward step looks it up once).
    // The leverage as mc::leverage_staircase computes it: the floor at 1e-4.
    QM_HOST_DEVICE inline double leverage_value(const mc::GridView<Real> &g, const LeverageCell &c)
    {
        const double lev = (1.0 - c.wK) * g.at(c.i0, c.j) + c.wK * g.at(c.i1, c.j);
        return lev < 1e-4 ? 1e-4 : lev;
    }

    QM_HOST_DEVICE inline double leverage_slope(const mc::GridView<Real> &g, const LeverageCell &c)
    {
        if (!c.free || !c.inside || c.dK <= 1e-12)
            return 0.0;
        return (g.at(c.i1, c.j) - g.at(c.i0, c.j)) / c.dK;
    }

    /// `a` times the two corners' shares, handed to add(parameter, adjoint).
    template <class Add>
    QM_HOST_DEVICE inline void leverage_accumulate_to(const LeverageCell &c, long nT, double a, int grid0, Add &&add)
    {
        if (!c.free)
            return;
        add(grid0 + c.i0 * nT + c.j, a * (1.0 - c.wK));
        add(grid0 + c.i1 * nT + c.j, a * c.wK);
    }

    QM_HOST_DEVICE inline void leverage_accumulate(const LeverageCell &c, long nT, double a, double *base, long stride,
                                                   int grid0)
    {
        leverage_accumulate_to(c, nT, a, grid0, [&](long p, double v)
                               { base[p * stride] += v; });
    }

    QM_HOST_DEVICE inline double leverage_slope(const mc::GridView<Real> &g, double S, double t)
    {
        const LeverageCell c = leverage_cell(g, S, t);
        if (!c.free || !c.inside || c.dK <= 1e-12)
            return 0.0;
        return (g.at(c.i1, c.j) - g.at(c.i0, c.j)) / c.dK;
    }

    QM_HOST_DEVICE inline void leverage_accumulate(const mc::GridView<Real> &g, double S, double t, double a,
                                                   double *base, long stride, int grid0)
    {
        leverage_accumulate(leverage_cell(g, S, t), g.nT, a, base, stride, grid0);
    }

} // namespace quantModeling::scripting::ir::detail

#endif // QM_SCRIPTING_IR_GRID_HPP
