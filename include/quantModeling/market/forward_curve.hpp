#ifndef MARKET_FORWARD_CURVE_HPP
#define MARKET_FORWARD_CURVE_HPP

#include "quantModeling/core/types.hpp"
#include "quantModeling/market/discount_curve.hpp"

#include <cstddef>
#include <utility>
#include <vector>

namespace quantModeling
{

    /**
     * @brief An equity forward curve: the spot, a discount curve and the
     *        forward F(t) on pillars (blueprint/wp/32-forward-curve.md).
     *
     * The one object every equity model reads for its carry and its
     * discounting, in place of a scalar rate and dividend yield.
     *
     *   y(t) = ln F(t) / S0, linear in t between pillars, from y(0) = 0 to
     *          the first pillar (a piecewise-constant carry rate), and the
     *          last segment's carry rate held flat past the last pillar.
     *   carry(t1, t2) = y(t2) − y(t1)              = ln F(t2) / F(t1)
     *   rate(t1, t2)  = ln DF(t1) − ln DF(t2)      (the integrated short rate)
     *   dividend(t1, t2) = rate − carry            (the integrated dividend
     *                                               yield, repo included)
     *
     * A discrete dividend is a jump in F: two pillars a short time apart
     * around the ex-date. The last two pillars must not be such a pair, since
     * their carry rate is extrapolated past the last one.
     *
     * The flat curve, built with flat(), keeps (r, q) and evaluates the same
     * expressions the models use with scalars: forward(t) = S0 e^{(r − q) t},
     * discount(t) = e^{−r t}, carry(t1, t2) = (r − q)(t2 − t1). A model may
     * keep its scalar path when is_flat(), which is how today's prices stay
     * bit for bit.
     */
    class ForwardCurve
    {
      public:
        /// The flat curve of a scalar rate and dividend yield.
        static ForwardCurve flat(Real spot, Real rate, Real dividend);

        /// Forwards on their own pillars, discounting on any curve.
        ForwardCurve(Real spot, DiscountCurve discount, std::vector<Time> times,
                     std::vector<Real> forwards);

        /// Forwards and discount factors on the same pillars (the market's
        /// expiries): the discount curve is log-linear between them with a
        /// flat forward rate outside, like the forwards.
        static ForwardCurve from_pillars(Real spot, std::vector<Time> times, std::vector<Real> forwards,
                                         std::vector<Real> discount_factors);

        Real spot() const { return spot_; }
        Real forward(Time t) const;
        Real discount(Time t) const { return discount_.discount(t); }

        /// ln F(t) / S0.
        Real log_forward(Time t) const;
        /// ln F(t2) / F(t1).
        Real carry(Time t1, Time t2) const;
        /// ln DF(t1) / DF(t2).
        Real rate(Time t1, Time t2) const;
        /// rate(t1, t2) − carry(t1, t2).
        Real dividend(Time t1, Time t2) const;

        /// The curve under parallel shifts of its rate and its dividend
        /// yield: DF(t) e^{−dr t} and F(t) e^{(dr − dq) t}, exact on the
        /// pillars and in a flat-forward extrapolation (a discount curve
        /// that holds its discount factor flat past its pillars shifts
        /// there by e^{−dr t_n} only). What a bumped rho is computed on.
        ForwardCurve shifted(Real rate_shift, Real dividend_shift) const;

        bool is_flat() const { return flat_; }
        /// The scalars of a flat curve (zero otherwise).
        Real flat_rate() const { return flat_rate_; }
        Real flat_dividend() const { return flat_dividend_; }

        const DiscountCurve &discount_curve() const { return discount_; }
        /// The forward pillars (empty for a flat curve).
        const std::vector<Time> &pillar_times() const { return times_; }
        /// ln F(t_i) / S0 on the pillars.
        const std::vector<Real> &pillar_log_forwards() const { return log_fwd_; }

        /// log_forward(t) as a linear combination of the pillars' log
        /// forwards, Σ weight · y(t_index), extrapolation included (empty for
        /// t ≤ 0 or a flat curve). What the risks per forward pillar need
        /// (WP 32 F3), as DiscountCurve::log_discount_weights does for rho.
        void log_forward_weights(Time t, std::vector<std::pair<std::size_t, Real>> &weights) const;

      private:
        ForwardCurve(Real spot, Real rate, Real dividend);

        Real spot_ = 0.0;
        DiscountCurve discount_;
        std::vector<Time> times_;
        std::vector<Real> log_fwd_;
        bool flat_ = false;
        Real flat_rate_ = 0.0;
        Real flat_dividend_ = 0.0;
    };

} // namespace quantModeling

#endif
