#ifndef QM_MODELS_EQUITY_KNOCK_OUT_HPP
#define QM_MODELS_EQUITY_KNOCK_OUT_HPP

#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

#include "quantModeling/aad/number.hpp"
#include "quantModeling/aad/tangent.hpp"
#include "quantModeling/core/types.hpp"
#include "quantModeling/core/sample.hpp"
#include "quantModeling/models/equity/last_step.hpp"
#include "quantModeling/utils/inverse_normal.hpp"
#include "quantModeling/utils/stats.hpp"

/**
 * @file knock_out.hpp
 * @brief The step into a knock-out date drawn conditioned on survival
 *        (knockOutAbove / knockOutBelow, issue #369; Glasserman & Staum 2001,
 *        Glasserman 2003 §7.2.3).
 *
 * The model's step into the date is lognormal given the path before it:
 *
 *     ln S' = ln S + m + s z,   z ~ N(0, 1).
 *
 * The path survives when S' stays inside the corridor (lo, hi) -- below the
 * knockOutAbove levels, above the knockOutBelow ones -- that is when
 * dl < z < du, dl = (ln lo - ln S - m) / s, du = (ln hi - ln S - m) / s. Its
 * probability given the path is
 *
 *     p = N(du) - N(dl),
 *
 * and z is drawn from the normal truncated to (dl, du) by inverting the
 * uniform the model was given: u = N(z), z' = N^-1(N(dl) + u p). The path
 * then never dies, it carries the weight p, and the payoff is multiplied by
 * the product of its weights: the same expectation (the tower property),
 * with no indicator left, so the pathwise derivatives are those of a smooth
 * function, and two coupled levels of multilevel no longer flip across the
 * barrier. z' is a monotone function of z, so a Sobol point and the Brownian
 * bridge keep their structure: only the one increment is transformed, and
 * the increments of a Brownian path stay independent.
 *
 * Generic in the number type: z' is N^-1 of a number that depends on the
 * parameters (through dl, du and p); its value is the double inverse, and
 * its derivatives come from Newton's step on N(z') = c (two steps: exact to
 * second order, for the second order's Tangent<Number>).
 */

namespace quantModeling::mc
{

    /// The knock-outs of one event on one asset, as a corridor in the spot.
    struct KnockOutCorridor
    {
        double lo = 0.0;                                     ///< knocked out at or below; 0: none
        double hi = std::numeric_limits<double>::infinity(); ///< knocked out at or above; inf: none
        std::ptrdiff_t slot = -1;                            ///< the first KnockOut slot, -1: none

        bool active() const { return slot >= 0; }
    };

    inline KnockOutCorridor knock_out_corridor(const SampleDef &def, std::size_t asset = 0)
    {
        KnockOutCorridor c;
        for (std::size_t k = 0; k < def.monitors.size(); ++k)
        {
            const StepMonitor &m = def.monitors[k];
            if (m.kind != StepQuantity::KnockOut || m.asset != asset)
                continue;
            if (c.slot < 0)
                c.slot = static_cast<std::ptrdiff_t>(k);
            if (m.above)
                c.hi = std::min(c.hi, m.level);
            else
                c.lo = std::max(c.lo, m.level);
        }
        return c;
    }

    inline bool asks_knock_out(const SampleDef &def)
    {
        for (const StepMonitor &m : def.monitors)
            if (m.kind == StepQuantity::KnockOut)
                return true;
        return false;
    }

    /// Whether the defline has a knock-out; throws where one shares its date
    /// with a lastStep*() expectation, whose Black formula assumes the
    /// unconditioned lognormal step.
    inline bool check_knock_outs(const std::vector<SampleDef> &defline, const char *model)
    {
        bool any = false;
        for (const SampleDef &d : defline)
            if (asks_knock_out(d))
            {
                any = true;
                if (asks_last_step(d))
                    throw InvalidInput(std::string(model) +
                                       ": knockOutAbove() / knockOutBelow() and lastStepCall() / lastStepPut() / "
                                       "lastStepDigital() on the same date: the knock-out conditions the step the "
                                       "last-step expectation integrates over");
            }
        return any;
    }

    /// The survival probability into the corridor's first slot, 1 into the
    /// other knock-out slots of the event (they multiply the same weight).
    template <class T>
    void write_knock_out(const SampleDef &def, Sample<T> &smp, const KnockOutCorridor &c, const T &p)
    {
        smp.monitored.resize(def.monitors.size());
        for (std::size_t k = 0; k < def.monitors.size(); ++k)
            if (def.monitors[k].kind == StepQuantity::KnockOut)
                smp.monitored[k] = static_cast<std::ptrdiff_t>(k) == c.slot ? p : T(1.0);
    }

    namespace knock_out_detail
    {
        constexpr double kInvSqrt2Pi = 0.39894228040143267794;

        template <class T>
        T pdf_of(const T &x)
        {
            using std::exp;
            return kInvSqrt2Pi * exp(-0.5 * x * x);
        }

        /// N^-1(c) with the derivatives of c. c's value is c0.
        template <class T>
        T inverse_normal_of(const T &c, double c0)
        {
            const double z0 = inverse_normal_cdf(c0);
            if constexpr (std::is_same_v<T, double>)
                return z0;
            else
            {
                using last_step_detail::normal_cdf_of;
                const double phi0 = kInvSqrt2Pi * std::exp(-0.5 * z0 * z0);
                T z = T(z0) + (c - c0) / phi0;
                // Newton again, its residual's value removed (it is rounding):
                // the value stays z0, the second derivatives become exact.
                const T g = normal_cdf_of(z) - c;
                return z - (g - to_double(g)) / pdf_of(z);
            }
        }
    } // namespace knock_out_detail

    /**
     * @brief Draws the step's normal conditioned on survival and returns
     *        the survival probability p.
     *
     * @param log_start ln S at the start of the conditioned (part of the) step
     * @param mean      its log-mean m
     * @param sd        its log-volatility s (> 0)
     * @param z         the unconditioned draw; replaced by the conditioned one
     */
    template <class T>
    T condition_on_survival(const KnockOutCorridor &c, const T &log_start, const T &mean, const T &sd, T &z)
    {
        using knock_out_detail::inverse_normal_of;
        using last_step_detail::normal_cdf_of;
        using std::log;

        const bool has_lo = c.lo > 0.0, has_hi = std::isfinite(c.hi);
        // N(dl), N(du) and their complements, each by its own N (no 1 - x).
        T A = T(0.0), Ac = T(1.0), B = T(1.0), Bc = T(0.0);
        if (has_lo)
        {
            const T dl = (log(c.lo) - log_start - mean) / sd;
            A = normal_cdf_of(dl);
            Ac = normal_cdf_of(T(-dl));
        }
        if (has_hi)
        {
            const T du = (log(c.hi) - log_start - mean) / sd;
            B = normal_cdf_of(du);
            Bc = normal_cdf_of(T(-du));
        }
        // p = B - A, from the smaller pair (Ac - Bc in the upper tail).
        const T p = to_double(A) < 0.5 ? T(B - A) : T(Ac - Bc);
        const double p0 = to_double(p);
        // Dead below 1e-14: its payments would weigh less than 1e-14 of
        // themselves, and the conditioned draw then stays where the normal
        // density is representable (u p >= ~1e-31, so |z'| < ~12): beyond,
        // N^-1's derivative 1 / phi(z') overflows.
        if (!(p0 > 1e-14))
            return T(0.0);

        const double z_in = to_double(z);
        const double u = norm_cdf(z_in), uc = norm_cdf(-z_in);
        if (u <= 0.5)
        {
            const T cdf = A + u * p; // N(z'), the lower tail
            z = inverse_normal_of(cdf, to_double(cdf));
        }
        else
        {
            const T sf = Bc + uc * p; // 1 - N(z'), the upper tail
            z = -inverse_normal_of(sf, to_double(sf));
        }
        return p;
    }

} // namespace quantModeling::mc

#endif // QM_MODELS_EQUITY_KNOCK_OUT_HPP
