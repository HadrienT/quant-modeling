#ifndef QM_SRC_ENGINES_XVA_ADJOINT_SUPPORT_HPP
#define QM_SRC_ENGINES_XVA_ADJOINT_SUPPORT_HPP

// What the adjoint runs of the exposure engines share (lot X8 of
// blueprint/wp/23-xva.md, one rate; §14.17, two factors): the curve and the
// Hull-White transition as functions of their inputs, the batches of paths
// and their tape. Internal to src/engines/xva.

#include "quantModeling/aad/number.hpp"
#include "quantModeling/aad/tape.hpp"
#include "quantModeling/core/types.hpp"
#include "quantModeling/engines/xva/xva_risks.hpp"
#include "quantModeling/risk/exposure_paths.hpp"
#include "quantModeling/utils/accumulators.hpp"
#include "quantModeling/utils/thread_pool.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <vector>

namespace quantModeling::xva_adjoint
{
    using aad::Number;

    inline constexpr std::size_t kBatch = 64;

    inline double expm1_of(double x)
    {
        return std::expm1(x);
    }
    /// exp(x) - 1 on the tape: the derivative is what matters here, and
    /// the cancellation costs a relative 1e-13 on the value at worst.
    inline Number expm1_of(const Number &x)
    {
        return Number(exp(x) - 1.0);
    }
    inline double value_of(double x)
    {
        return x;
    }
    inline double value_of(const Number &x)
    {
        return x.value();
    }

    /**
     * ln P(0, t) of a curve given by the zero rates of its pillars
     * (`log_df[k]` = -zero[k] × pillar k): DiscountCurve::discount, in T.
     * Without pillars the curve is flat at zero[0].
     */
    template <class T>
    T curve_log_discount(const std::vector<Time> &ts, bool flat_forward, const std::vector<T> &zero,
                         const std::vector<T> &log_df, Time t)
    {
        if (ts.empty())
            return T(-zero[0] * t);
        const std::size_t n = ts.size();
        if (t <= ts.front())
            return flat_forward ? T(log_df[0] * (t / ts.front())) : log_df[0];
        if (t >= ts.back())
        {
            if (!flat_forward)
                return log_df[n - 1];
            if (n == 1)
                return T(log_df[0] + log_df[0] / ts[0] * (t - ts.back()));
            return T(log_df[n - 1] +
                     (log_df[n - 1] - log_df[n - 2]) / (ts[n - 1] - ts[n - 2]) * (t - ts.back()));
        }
        const std::size_t idx =
            static_cast<std::size_t>(std::upper_bound(ts.begin(), ts.end(), t) - ts.begin());
        const Real w = (t - ts[idx - 1]) / (ts[idx] - ts[idx - 1]);
        return T((1.0 - w) * log_df[idx - 1] + w * log_df[idx]);
    }

    template <class T>
    struct HullWhiteTransition
    {
        T decay, drift, variance;
    };

    /// HullWhiteCurveModel::transition, in T: x(s) -> x(t) under the
    /// forward measure of `numeraire_maturity`.
    template <class T>
    HullWhiteTransition<T> hull_white_transition(const T &a, const T &sigma, Time s, Time t,
                                                 Time numeraire_maturity)
    {
        using std::exp;
        const T s2 = sigma * sigma;
        const Time d = t - s;
        const T em = expm1_of(T(-a * d));
        const T ep = expm1_of(T(a * d));
        const T e2 = expm1_of(T(-2.0 * a * d));
        const T I1 = s2 / (2.0 * a * a) * (-em - exp(-2.0 * a * t) * ep);
        const T I2 = (-em + 0.5 * exp(-a * (numeraire_maturity - t)) * e2) / (a * a);
        return {T(exp(-a * d)), T(I1 - s2 * I2), T(s2 * (-e2) / (2.0 * a))};
    }

    /// ln S(t) of a piecewise-constant hazard curve: CreditCurve::survival.
    template <class T>
    T log_survival(const std::vector<Time> &times, const std::vector<T> &hazards, Time t)
    {
        if (t <= 0.0)
            return T(0.0);
        T cumulative(0.0);
        Time start = 0.0;
        for (std::size_t i = 0; i < times.size() && i < hazards.size(); ++i)
        {
            if (t <= times[i])
                return T(-(cumulative + hazards[i] * (t - start)));
            cumulative = cumulative + hazards[i] * (times[i] - start);
            start = times[i];
        }
        return T(-(cumulative + hazards.back() * (t - start)));
    }

    /// The mean over the paths and its standard error, from the means of
    /// the batches, in batch order. The last batch may be short: it
    /// weighs its paths in the mean; the error is the dispersion of the
    /// batch means.
    inline Estimate over_batches(const std::vector<Real> &batch_means, std::size_t paths)
    {
        WelfordAccumulator acc;
        Real sum = 0.0;
        for (std::size_t b = 0; b < batch_means.size(); ++b)
        {
            acc.add(batch_means[b]);
            sum += batch_means[b] * static_cast<Real>(std::min(kBatch, paths - b * kBatch));
        }
        return {sum / static_cast<Real>(paths), acc.std_error()};
    }

    /// The tape, and the multi-adjoint mode, of a differentiable run on
    /// this thread; what was there before comes back on the way out.
    struct TapeScope
    {
        aad::Tape *tape_before = Number::tape;
        bool multi_before = aad::Tape::is_multi();
        std::size_t num_adj_before = aad::Node::num_adj;

        explicit TapeScope(aad::Tape *tape)
        {
            Number::tape = tape;
            aad::Tape::set_multi(true);
            aad::Node::num_adj = kXvaOutputs;
        }
        ~TapeScope()
        {
            Number::tape = tape_before;
            aad::Tape::set_multi(multi_before);
            aad::Node::num_adj = num_adj_before;
        }
        TapeScope(const TapeScope &) = delete;
        TapeScope &operator=(const TapeScope &) = delete;
    };

    /// Runs the batches, on the pool when there is one; a batch is
    /// run(batch, first_path, size) and writes its own slot.
    template <class Run>
    void for_each_batch(std::size_t paths, ThreadPool *pool, const Run &run)
    {
        const std::size_t batches = (paths + kBatch - 1) / kBatch;
        const auto one = [&](std::size_t b)
        {
            const std::size_t first = b * kBatch;
            run(b, first, std::min(kBatch, paths - first));
            return true;
        };
        if (pool == nullptr || pool->num_threads() == 0)
        {
            for (std::size_t b = 0; b < batches; ++b)
                one(b);
            return;
        }
        std::vector<TaskHandle> handles;
        handles.reserve(batches);
        for (std::size_t b = 0; b < batches; ++b)
            handles.push_back(pool->spawn_task([&one, b]
                                               { return one(b); }));
        std::exception_ptr failure;
        for (TaskHandle &h : handles)
        {
            try
            {
                pool->active_wait(h);
            }
            catch (...)
            {
                if (!failure)
                    failure = std::current_exception();
            }
        }
        if (failure)
            std::rethrow_exception(failure);
    }
} // namespace quantModeling::xva_adjoint

#endif // QM_SRC_ENGINES_XVA_ADJOINT_SUPPORT_HPP
