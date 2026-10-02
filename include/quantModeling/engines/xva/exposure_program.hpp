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
 *
 * The coefficients and the evaluation are written for any number type T: a
 * double on the host and on the device, and the differentiable number of the
 * adjoint run (lot X8, engines/xva/xva_risks.hpp), whose coefficients are
 * recorded on the tape as functions of the curve and of the model. For that
 * run each coefficient also keeps its **source**: the dates it was computed
 * from, so that it can be computed again in another number type.
 */

namespace quantModeling::xva
{

    /// amount × exp(-G x).
    template <class T>
    struct BasicProgramExp
    {
        T amount;
        T G;
    };

    /// An amount fixed at `fixing` (a grid index, or -1 for today, where the
    /// state is 0): scale × (B exp(g x) + constant).
    template <class T>
    struct BasicProgramCoupon
    {
        T B;
        T g;
        Real constant;
        Real scale;
        int fixing;
        int padding = 0;
    };

    /// A coupon already fixed, discounted from its payment: coupon × amount ×
    /// exp(-G x).
    template <class T>
    struct BasicProgramAccrued
    {
        T amount;
        T G;
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
    template <class T>
    struct BasicProgramOptionDate
    {
        T decay;
        T drift;
        T variance;
        T sd;
        /// (variance - y(T)) / 2.
        T convexity;
        T zcb_amount;
        T zcb_G;
    };

    /// One bond of the exercise value: coefficient × exp(-G x(T) - G² y(T) / 2).
    template <class T>
    struct BasicProgramOptionBond
    {
        T coefficient;
        T G;
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

    using ProgramExp = BasicProgramExp<Real>;
    using ProgramCoupon = BasicProgramCoupon<Real>;
    using ProgramAccrued = BasicProgramAccrued<Real>;
    using ProgramOptionDate = BasicProgramOptionDate<Real>;
    using ProgramOptionBond = BasicProgramOptionBond<Real>;

    /// The arrays, as the pointers a kernel takes by value.
    template <class T>
    struct BasicProgramView
    {
        const ProgramTrade *trades = nullptr;
        const ProgramDate *dates = nullptr;
        const BasicProgramExp<T> *exps = nullptr;
        const BasicProgramCoupon<T> *coupons = nullptr;
        const BasicProgramAccrued<T> *accrued = nullptr;
        const int *paid = nullptr;
        const BasicProgramOptionDate<T> *option_dates = nullptr;
        const BasicProgramOptionBond<T> *option_bonds = nullptr;
        const ProgramInterval *intervals = nullptr;
        int n_trades = 0;
        int n_dates = 0;
    };

    using ProgramView = BasicProgramView<Real>;

    // ── Where each coefficient comes from (lot X8) ──────────────────────────
    //
    // With P the discount curve, G and y the model's functions:

    /// raw × P(to) / P(from) × exp(-G(from, to)² y(from) / 2), with
    /// G(from, to): a cash flow `raw` at `to`, seen from `from`.
    struct BondSource
    {
        Real raw;
        Time from;
        Time to;
    };

    /// B = beta × P(start) / P(end) × exp(g² y(start) / 2), g = G(start, end).
    struct CouponSource
    {
        Real beta;
        Time start;
        Time end;
    };

    /// The transition of the state from `from` to `expiry` under the
    /// expiry-forward measure, and P(from, expiry).
    struct OptionDateSource
    {
        Time from;
        Time expiry;
    };

    /// coefficient = raw × P(to) / P(expiry), G = G(expiry, to).
    struct OptionBondSource
    {
        Real raw;
        Time expiry;
        Time to;
    };

    /// What a trade is worth today, for the collateral called on it: the
    /// sum of raw × P(to) over `terms`, or, for an option, its integral seen
    /// from today (`expiry` > 0).
    struct TodaySource
    {
        int term_begin = 0, term_end = 0;
        Time expiry = -1.0;
    };

    struct TodayTerm
    {
        Real raw;
        Time to;
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

        /// The sources, index for index with the arrays above; never sent to
        /// a device.
        std::vector<BondSource> exp_sources;
        std::vector<CouponSource> coupon_sources;
        std::vector<BondSource> accrued_sources;
        std::vector<OptionDateSource> option_date_sources;
        std::vector<OptionBondSource> option_bond_sources;
        /// One per trade.
        std::vector<TodaySource> today;
        std::vector<TodayTerm> today_terms;

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

    /// Φ(x), as utils/stats.hpp writes it. A differentiable number finds its
    /// own normal_cdf by argument-dependent lookup.
    QM_HOST_DEVICE inline Real normal_cdf(Real x)
    {
        using std::erfc;
        return 0.5 * erfc(-x / 1.4142135623730951);
    }

    /// max(x, 0).
    template <class T>
    QM_HOST_DEVICE inline T program_positive(const T &x)
    {
        if (x > 0.0)
            return x;
        return T(0.0);
    }

    template <class T>
    QM_HOST_DEVICE inline T program_coupon(const BasicProgramCoupon<T> &c, const T *state)
    {
        using std::exp;
        if (c.fixing < 0)
            return T(c.scale * (c.B * exp(c.g * 0.0) + c.constant));
        return T(c.scale * (c.B * exp(c.g * state[c.fixing]) + c.constant));
    }

    /// The part of a trade that is a sum of bonds, at grid date i.
    template <class T>
    QM_HOST_DEVICE inline T program_linear_value(const BasicProgramView<T> &p, const ProgramTrade &trade, int i,
                                                 const T *state)
    {
        using std::exp;
        const ProgramDate &date = p.dates[trade.date_begin + i];
        const T &x = state[i];
        T v(0.0);
        // Four terms to a statement, added in the order of one term at a
        // time: the same double, and a quarter of the nodes on a tape.
        int j = date.exp_begin;
        for (; j + 4 <= date.exp_end; j += 4)
            v = v + p.exps[j].amount * exp(-p.exps[j].G * x) + p.exps[j + 1].amount * exp(-p.exps[j + 1].G * x) +
                p.exps[j + 2].amount * exp(-p.exps[j + 2].G * x) + p.exps[j + 3].amount * exp(-p.exps[j + 3].G * x);
        for (; j < date.exp_end; ++j)
            v += p.exps[j].amount * exp(-p.exps[j].G * x);
        for (j = date.accrued_begin; j < date.accrued_end; ++j)
            v += program_coupon(p.coupons[p.accrued[j].coupon], state) * p.accrued[j].amount *
                 exp(-p.accrued[j].G * x);
        return v;
    }

    template <class T>
    QM_HOST_DEVICE inline T program_linear_cashflow(const BasicProgramView<T> &p, const ProgramTrade &trade,
                                                    int i, const T *state)
    {
        const ProgramDate &date = p.dates[trade.date_begin + i];
        T flow(date.fixed_cashflow);
        for (int j = date.paid_begin; j < date.paid_end; ++j)
            flow += program_coupon(p.coupons[p.paid[j]], state);
        return flow;
    }

    /// The integral of an option's exercise value over its exercise region,
    /// bond by bond, seen from a date where the state is x.
    template <class T>
    QM_HOST_DEVICE inline T program_option_integral(const BasicProgramView<T> &p, const ProgramTrade &trade,
                                                    const BasicProgramOptionDate<T> &d, const T &x)
    {
        using std::exp;
        using std::isinf;
        const T m = d.decay * x + d.drift;
        const T &v = d.variance;
        const T &sd = d.sd;
        T value(0.0);
        for (int r = trade.interval_begin; r < trade.interval_end; ++r)
        {
            const Real l = p.intervals[r].lower, u = p.intervals[r].upper;
            // One statement a bond, whichever bounds are infinite: Φ(+∞) = 1
            // and Φ(-∞) = 0 are written in, not computed.
            const bool above = isinf(u), below = isinf(l);
            for (int k = trade.bond_begin; k < trade.bond_end; ++k)
            {
                const T &G = p.option_bonds[k].G;
                const T &c = p.option_bonds[k].coefficient;
                if (above && below)
                    value += c * exp(-G * m + G * G * d.convexity) * (1.0 - 0.0);
                else if (above)
                    value += c * exp(-G * m + G * G * d.convexity) * (1.0 - normal_cdf((l - m + G * v) / sd));
                else if (below)
                    value += c * exp(-G * m + G * G * d.convexity) * (normal_cdf((u - m + G * v) / sd) - 0.0);
                else
                    value += c * exp(-G * m + G * G * d.convexity) *
                             (normal_cdf((u - m + G * v) / sd) - normal_cdf((l - m + G * v) / sd));
            }
        }
        // Deep out of the money the terms cancel down to rounding noise.
        return program_positive(T(d.zcb_amount * exp(-d.zcb_G * x) * value));
    }

    /// The option at grid date i < expiry.
    template <class T>
    QM_HOST_DEVICE inline T program_option_value(const BasicProgramView<T> &p, const ProgramTrade &trade, int i,
                                                 const T *state)
    {
        return program_option_integral(p, trade, p.option_dates[trade.option_date_begin + i], state[i]);
    }

    /// What the option became at its expiry: true when it was exercised, and
    /// for a trade that is not an option.
    template <class T>
    QM_HOST_DEVICE inline bool program_exercised(const BasicProgramView<T> &p, const ProgramTrade &trade,
                                                 const T *state)
    {
        return trade.expiry < 0 || program_linear_value(p, trade, trade.expiry, state) > 0.0;
    }

    /// Value of one unit of the trade at grid date i, of the cash flows
    /// strictly after that date. `exercised` is program_exercised(), read
    /// once per path (it only matters from the expiry on).
    template <class T>
    QM_HOST_DEVICE inline T program_value(const BasicProgramView<T> &p, const ProgramTrade &trade, int i,
                                          const T *state, bool exercised)
    {
        if (i < trade.expiry)
            return program_option_value(p, trade, i, state);
        if (exercised)
            return program_linear_value(p, trade, i, state);
        return T(0.0);
    }

    /// Cash flow of one unit of the trade at grid date i.
    template <class T>
    QM_HOST_DEVICE inline T program_cashflow(const BasicProgramView<T> &p, const ProgramTrade &trade, int i,
                                             const T *state, bool exercised)
    {
        if (trade.expiry >= 0 && (i <= trade.expiry || !exercised))
            return T(0.0);
        return program_linear_cashflow(p, trade, i, state);
    }

    /**
     * @brief How the model state moves on the grid and what each path weighs:
     *        x(t_i) = decay_i x(t_{i-1}) + drift_i + sd_i Z_i, x(0) = 0, and
     *        the discount weight scale_i exp(G_i x(t_i)).
     *
     * Draw i of path p is Φ⁻¹(Philox(seed, p, i)).
     */
    template <class T>
    struct BasicStateDynamics
    {
        std::vector<T> decay, drift, sd;
        std::vector<T> weight_scale, weight_G;
    };

    using StateDynamics = BasicStateDynamics<Real>;

} // namespace quantModeling::xva

#endif // QM_ENGINES_XVA_EXPOSURE_PROGRAM_HPP
