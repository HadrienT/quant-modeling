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

    /// The four corners and weights of the local vol at (S, t), and whether
    /// the floor leaves it free (mc::local_vol_bilinear's arithmetic).
    struct GridCell
    {
        int i0, i1, j0, j1;
        double wK, wT, dK;
        double sigma; ///< the bilinear value, before the floor
        bool inside, free;
    };

    QM_HOST_DEVICE inline GridCell grid_cell(const mc::GridView<Real> &g, double S, double t)
    {
        GridCell c{};
        const double Sc = S < g.K[0] ? g.K[0] : (S > g.K[g.nK - 1] ? g.K[g.nK - 1] : S);
        c.inside = Sc == S;
        mc::bracket(g.K, g.nK, Sc, c.i0, c.i1);
        const double K0 = g.K[c.i0];
        c.dK = g.K[c.i1] - K0;
        c.wK = c.dK <= 1e-12 ? 0.0 : (c.inside ? (S - K0) / c.dK : (Sc - K0) / c.dK);
        const double tc = t < g.T_grid[0] ? g.T_grid[0] : (t > g.T_grid[g.nT - 1] ? g.T_grid[g.nT - 1] : t);
        mc::bracket(g.T_grid, g.nT, tc, c.j0, c.j1);
        const double T0 = g.T_grid[c.j0];
        const double dT = g.T_grid[c.j1] - T0;
        c.wT = (dT > 1e-12) ? (tc - T0) / dT : 0.0;
        c.sigma = (1.0 - c.wK) * (1.0 - c.wT) * g.at(c.i0, c.j0) + c.wK * (1.0 - c.wT) * g.at(c.i1, c.j0) +
                  (1.0 - c.wK) * c.wT * g.at(c.i0, c.j1) + c.wK * c.wT * g.at(c.i1, c.j1);
        c.free = c.sigma > 1e-6;
        return c;
    }

    // The same from a cell already found: a backward step looks its cell up
    // once for the vol, its slope and the corners' shares.
    QM_HOST_DEVICE inline double grid_slope(const mc::GridView<Real> &g, const GridCell &c)
    {
        if (!c.free || !c.inside || c.dK <= 1e-12)
            return 0.0;
        return ((1.0 - c.wT) * (g.at(c.i1, c.j0) - g.at(c.i0, c.j0)) + c.wT * (g.at(c.i1, c.j1) - g.at(c.i0, c.j1))) /
               c.dK;
    }

    QM_HOST_DEVICE inline void grid_accumulate(const GridCell &c, long nT, double a, double *base, long stride,
                                               int grid0)
    {
        if (!c.free)
            return;
        base[(grid0 + c.i0 * nT + c.j0) * stride] += a * ((1.0 - c.wK) * (1.0 - c.wT));
        base[(grid0 + c.i1 * nT + c.j0) * stride] += a * (c.wK * (1.0 - c.wT));
        base[(grid0 + c.i0 * nT + c.j1) * stride] += a * ((1.0 - c.wK) * c.wT);
        base[(grid0 + c.i1 * nT + c.j1) * stride] += a * (c.wK * c.wT);
    }

    /// d sigma_loc / d S: 0 outside the strikes and where the floor binds.
    QM_HOST_DEVICE inline double grid_slope(const mc::GridView<Real> &g, double S, double t)
    {
        const GridCell c = grid_cell(g, S, t);
        if (!c.free || !c.inside || c.dK <= 1e-12)
            return 0.0;
        return ((1.0 - c.wT) * (g.at(c.i1, c.j0) - g.at(c.i0, c.j0)) + c.wT * (g.at(c.i1, c.j1) - g.at(c.i0, c.j1))) /
               c.dK;
    }

    /// `a` times each corner's weight, added to the corners' adjoints
    /// (parameter p at base[p * stride]).
    QM_HOST_DEVICE inline void grid_accumulate(const mc::GridView<Real> &g, double S, double t, double a,
                                               double *base, long stride, int grid0)
    {
        const GridCell c = grid_cell(g, S, t);
        if (!c.free)
            return;
        const long nT = g.nT;
        base[(grid0 + c.i0 * nT + c.j0) * stride] += a * ((1.0 - c.wK) * (1.0 - c.wT));
        base[(grid0 + c.i1 * nT + c.j0) * stride] += a * (c.wK * (1.0 - c.wT));
        base[(grid0 + c.i0 * nT + c.j1) * stride] += a * ((1.0 - c.wK) * c.wT);
        base[(grid0 + c.i1 * nT + c.j1) * stride] += a * (c.wK * c.wT);
    }

    /// The two corners of the SLV leverage at (S, t): linear in the strike,
    /// a staircase in time (mc::leverage_staircase).
    struct LeverageCell
    {
        int i0, i1, j;
        double wK, dK;
        bool inside, free;
    };

    QM_HOST_DEVICE inline LeverageCell leverage_cell(const mc::GridView<Real> &g, double S, double t)
    {
        LeverageCell c{};
        const double Sc = S < g.K[0] ? g.K[0] : (S > g.K[g.nK - 1] ? g.K[g.nK - 1] : S);
        c.inside = Sc == S;
        mc::bracket(g.K, g.nK, Sc, c.i0, c.i1);
        const double K0 = g.K[c.i0];
        c.dK = g.K[c.i1] - K0;
        c.wK = c.dK <= 1e-12 ? 0.0 : (c.inside ? (S - K0) / c.dK : (Sc - K0) / c.dK);
        const double tc = t < g.T_grid[0] ? g.T_grid[0] : (t > g.T_grid[g.nT - 1] ? g.T_grid[g.nT - 1] : t);
        const int j = mc::lower_index(g.T_grid, g.nT, tc);
        c.j = (j > 0) ? j - 1 : 0;
        c.free = (1.0 - c.wK) * g.at(c.i0, c.j) + c.wK * g.at(c.i1, c.j) > 1e-4;
        return c;
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
        const LeverageCell c = leverage_cell(g, S, t);
        if (!c.free)
            return;
        const long nT = g.nT;
        base[(grid0 + c.i0 * nT + c.j) * stride] += a * (1.0 - c.wK);
        base[(grid0 + c.i1 * nT + c.j) * stride] += a * c.wK;
    }

} // namespace quantModeling::scripting::ir::detail

#endif // QM_SCRIPTING_IR_GRID_HPP
