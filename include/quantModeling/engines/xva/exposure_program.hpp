#ifndef QM_ENGINES_XVA_EXPOSURE_PROGRAM_HPP
#define QM_ENGINES_XVA_EXPOSURE_PROGRAM_HPP

#include "quantModeling/core/platform.hpp"
#include "quantModeling/core/types.hpp"

#include <cmath>
#include <cstddef>
#include <vector>

/**
 * @file exposure_program.hpp
 * @brief The trades of an exposure simulation as flat arrays a GPU can read
 *        (blueprint/wp/23-xva.md §14.11, lot X7).
 *
 * The exposure engine asks each trade for its value through a virtual
 * interface (FutureValue), which a device cannot call. A trade with a closed
 * form under Hull-White is, however, made of three kinds of terms only, none
 * of which names a product:
 *
 *  - **exponentials of today's state**, amount × exp(-G x(t_i)): a cash flow
 *    times a zero-coupon bond;
 *  - **accrued amounts**, known from the state at an earlier date and paid
 *    later: scale × (B exp(g x(t_fixing)) + constant), times such a bond;
 *  - a **Gaussian integral** of exponentials over a set of intervals of the
 *    state at a later date, and a **gate** read at that date: an option
 *    before its expiry, and what it became after.
 *
 * FutureValue::compile() writes a trade in those terms; the functions below
 * evaluate them, on the host and on the device alike (QM_HOST_DEVICE). The
 * engine and the kernel still do not know what a swap is.
 *
 * Every sum runs in the order the trade's own value() uses, so that on the
 * host the program gives the trade's numbers, and on the device the same up
 * to the last bits of exp and erfc.
 */

namespace quantModeling::xva
{

    /// amount × exp(-G x).
    struct ProgramExp
    {
        Real amount;
        Real G;
    };

    /// An amount fixed at `fixing` (a grid index, or -1 for today, where the
    /// state is 0): scale × (B exp(g x) + constant).
    struct ProgramCoupon
    {
        Real B;
        Real g;
        Real constant;
        Real scale;
        int fixing;
        int padding = 0;
    };

    /// A coupon already fixed, discounted from its payment: coupon × amount ×
    /// exp(-G x).
    struct ProgramAccrued
    {
        Real amount;
        Real G;
        int coupon;
        int padding = 0;
    };

    /// What one trade is at one grid date.
    struct ProgramDate
    {
        /// Cash flow of the date known today.
        Real fixed_cashflow = 0.0;
        int exp_begin = 0, exp_end = 0;
        int accrued_begin = 0, accrued_end = 0;
        /// The coupons paid at this date, as a range of ExposureProgram::paid.
        int paid_begin = 0, paid_end = 0;
    };

    /// An option at a grid date before its expiry T: the state at T given
    /// the state x today is N(decay x + drift, variance) under the T-forward
    /// measure, and P(t, T | x) = zcb_amount × exp(-zcb_G x).
    struct ProgramOptionDate
    {
        Real decay;
        Real drift;
        Real variance;
        Real sd;
        /// (variance - y(T)) / 2.
        Real convexity;
        Real zcb_amount;
        Real zcb_G;
    };

    /// One bond of the exercise value: coefficient × exp(-G x(T) - G² y(T) / 2).
    struct ProgramOptionBond
    {
        Real coefficient;
        Real G;
    };

    /// An interval of x(T) where the option is exercised; the outer bounds
    /// may be infinite.
    struct ProgramInterval
    {
        Real lower;
        Real upper;
    };

    struct ProgramTrade
    {
        Real quantity = 1.0;
        /// First of the trade's ProgramDate, one per grid date.
        int date_begin = 0;
        /// Grid index of the expiry, or -1 for a trade that is not an option.
        int expiry = -1;
        /// First of the option's ProgramOptionDate, one per date before expiry.
        int option_date_begin = 0;
        int bond_begin = 0, bond_end = 0;
        int interval_begin = 0, interval_end = 0;
        int padding = 0;
    };

    /// The arrays, as the pointers a kernel takes by value.
    struct ProgramView
    {
        const ProgramTrade *trades = nullptr;
        const ProgramDate *dates = nullptr;
        const ProgramExp *exps = nullptr;
        const ProgramCoupon *coupons = nullptr;
        const ProgramAccrued *accrued = nullptr;
        const int *paid = nullptr;
        const ProgramOptionDate *option_dates = nullptr;
        const ProgramOptionBond *option_bonds = nullptr;
        const ProgramInterval *intervals = nullptr;
        int n_trades = 0;
        int n_dates = 0;
    };

    /// The trades of one simulation, on one grid.
    struct ExposureProgram
    {
        std::size_t dates = 0;
        std::vector<ProgramTrade> trades;
        std::vector<ProgramDate> date_records;
        std::vector<ProgramExp> exps;
        std::vector<ProgramCoupon> coupons;
        std::vector<ProgramAccrued> accrued;
        std::vector<int> paid;
        std::vector<ProgramOptionDate> option_dates;
        std::vector<ProgramOptionBond> option_bonds;
        std::vector<ProgramInterval> intervals;

        ProgramView view() const
        {
            return {trades.data(),
                    date_records.data(),
                    exps.data(),
                    coupons.data(),
                    accrued.data(),
                    paid.data(),
                    option_dates.data(),
                    option_bonds.data(),
                    intervals.data(),
                    static_cast<int>(trades.size()),
                    static_cast<int>(dates)};
        }
    };

    /// Φ(x), as utils/stats.hpp writes it.
    QM_HOST_DEVICE inline Real program_norm_cdf(Real x)
    {
        using std::erfc;
        return 0.5 * erfc(-x / 1.4142135623730951);
    }

    QM_HOST_DEVICE inline Real program_coupon(const ProgramCoupon &c, const Real *state)
    {
        using std::exp;
        const Real x = c.fixing < 0 ? 0.0 : state[c.fixing];
        return c.scale * (c.B * exp(c.g * x) + c.constant);
    }

    /// The part of a trade that is a sum of bonds, at grid date i.
    QM_HOST_DEVICE inline Real program_linear_value(const ProgramView &p, const ProgramTrade &trade,
                                                    int i, const Real *state)
    {
        using std::exp;
        const ProgramDate &date = p.dates[trade.date_begin + i];
        const Real x = state[i];
        Real v = 0.0;
        for (int j = date.exp_begin; j < date.exp_end; ++j)
            v += p.exps[j].amount * exp(-p.exps[j].G * x);
        for (int j = date.accrued_begin; j < date.accrued_end; ++j)
            v += program_coupon(p.coupons[p.accrued[j].coupon], state) * p.accrued[j].amount *
                 exp(-p.accrued[j].G * x);
        return v;
    }

    QM_HOST_DEVICE inline Real program_linear_cashflow(const ProgramView &p, const ProgramTrade &trade,
                                                       int i, const Real *state)
    {
        const ProgramDate &date = p.dates[trade.date_begin + i];
        Real flow = date.fixed_cashflow;
        for (int j = date.paid_begin; j < date.paid_end; ++j)
            flow += program_coupon(p.coupons[p.paid[j]], state);
        return flow;
    }

    /// The option at grid date i < expiry: its exercise value integrated
    /// over the exercise region, bond by bond.
    QM_HOST_DEVICE inline Real program_option_value(const ProgramView &p, const ProgramTrade &trade,
                                                    int i, const Real *state)
    {
        using std::exp;
        using std::isinf;
        const ProgramOptionDate &d = p.option_dates[trade.option_date_begin + i];
        const Real x = state[i];
        const Real m = d.decay * x + d.drift, v = d.variance, sd = d.sd;
        Real value = 0.0;
        for (int r = trade.interval_begin; r < trade.interval_end; ++r)
        {
            const Real l = p.intervals[r].lower, u = p.intervals[r].upper;
            for (int k = trade.bond_begin; k < trade.bond_end; ++k)
            {
                const Real G = p.option_bonds[k].G;
                const Real Fu = isinf(u) ? 1.0 : program_norm_cdf((u - m + G * v) / sd);
                const Real Fl = isinf(l) ? 0.0 : program_norm_cdf((l - m + G * v) / sd);
                value += p.option_bonds[k].coefficient * exp(-G * m + G * G * d.convexity) * (Fu - Fl);
            }
        }
        const Real priced = d.zcb_amount * exp(-d.zcb_G * x) * value;
        // Deep out of the money the terms cancel down to rounding noise.
        return priced > 0.0 ? priced : 0.0;
    }

    /// What the option became at its expiry: true when it was exercised, and
    /// for a trade that is not an option.
    QM_HOST_DEVICE inline bool program_exercised(const ProgramView &p, const ProgramTrade &trade,
                                                 const Real *state)
    {
        return trade.expiry < 0 || program_linear_value(p, trade, trade.expiry, state) > 0.0;
    }

    /// Value of one unit of the trade at grid date i, of the cash flows
    /// strictly after that date. `exercised` is program_exercised(), read
    /// once per path (it only matters from the expiry on).
    QM_HOST_DEVICE inline Real program_value(const ProgramView &p, const ProgramTrade &trade, int i,
                                             const Real *state, bool exercised)
    {
        if (i < trade.expiry)
            return program_option_value(p, trade, i, state);
        return exercised ? program_linear_value(p, trade, i, state) : 0.0;
    }

    /// Cash flow of one unit of the trade at grid date i.
    QM_HOST_DEVICE inline Real program_cashflow(const ProgramView &p, const ProgramTrade &trade, int i,
                                                const Real *state, bool exercised)
    {
        if (trade.expiry >= 0 && (i <= trade.expiry || !exercised))
            return 0.0;
        return program_linear_cashflow(p, trade, i, state);
    }

    /**
     * @brief How the model state moves on the grid and what each path weighs:
     *        x(t_i) = decay_i x(t_{i-1}) + drift_i + sd_i Z_i, x(0) = 0, and
     *        the discount weight scale_i exp(G_i x(t_i)).
     *
     * Draw i of path p is Φ⁻¹(Philox(seed, p, i)).
     */
    struct StateDynamics
    {
        std::vector<Real> decay, drift, sd;
        std::vector<Real> weight_scale, weight_G;
    };

} // namespace quantModeling::xva

#endif // QM_ENGINES_XVA_EXPOSURE_PROGRAM_HPP
