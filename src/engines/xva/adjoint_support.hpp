#ifndef QM_SRC_ENGINES_XVA_ADJOINT_SUPPORT_HPP
#define QM_SRC_ENGINES_XVA_ADJOINT_SUPPORT_HPP

// What the adjoint runs of the exposure engines share (lot X8 of
// blueprint/wp/23-xva.md, one rate; §14.17, two factors): the curve and the
// Hull-White transition as functions of their inputs, the batches of paths
// and their tape. Internal to src/engines/xva.

#include "quantModeling/aad/number.hpp"
#include "quantModeling/aad/tape.hpp"
#include "quantModeling/core/types.hpp"
#include "quantModeling/engines/xva/exposure_program.hpp"
#include "quantModeling/engines/xva/xva_risks.hpp"
#include "quantModeling/risk/exposure_paths.hpp"
#include "quantModeling/utils/accumulators.hpp"
#include "quantModeling/utils/thread_pool.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <string>
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
    /// The coefficients of an exposure program (engines/xva/exposure_program.hpp)
    /// as functions of the inputs of one rate model.
    template <class T>
    struct ProgramSetup
    {
        std::vector<xva::BasicProgramExp<T>> exps;
        std::vector<xva::BasicProgramCoupon<T>> coupons;
        std::vector<xva::BasicProgramAccrued<T>> accrued;
        std::vector<xva::BasicProgramOptionDate<T>> option_dates;
        std::vector<xva::BasicProgramOptionBond<T>> option_bonds;

        xva::BasicProgramView<T> view(const xva::ExposureProgram &p) const
        {
            return {p.trades.data(),
                    p.date_records.data(),
                    exps.data(),
                    coupons.data(),
                    accrued.data(),
                    p.paid.data(),
                    option_dates.data(),
                    option_bonds.data(),
                    p.intervals.data(),
                    static_cast<int>(p.trades.size()),
                    static_cast<int>(p.dates)};
        }
    };

    /// An option seen from `from`: the state at its expiry under the
    /// expiry-forward measure, and the bond to it. `M` is a rate model in T:
    /// G(t, T), y(t), log_discount(t) and transition(s, t, numeraire).
    template <class T, class M>
    xva::BasicProgramOptionDate<T> program_option_date(M &m, Time from, Time expiry)
    {
        using std::exp;
        using std::sqrt;
        const auto tr = m.transition(from, expiry, expiry);
        const T G = m.G(from, expiry);
        return {tr.decay,
                tr.drift,
                tr.variance,
                T(sqrt(tr.variance)),
                T(0.5 * (tr.variance - m.y(expiry))),
                T(exp(m.log_discount(expiry) - m.log_discount(from) - 0.5 * G * G * m.y(from))),
                G};
    }

    /// Every coefficient of the program from its source, in T.
    template <class T, class M>
    void build_program(const xva::ExposureProgram &prog, M &m, ProgramSetup<T> &s)
    {
        using std::exp;
        const auto bond = [&m](const xva::BondSource &b)
        {
            const T G = m.G(b.from, b.to);
            return xva::BasicProgramExp<T>{
                T(b.raw * exp(m.log_discount(b.to) - m.log_discount(b.from) - 0.5 * G * G * m.y(b.from))), G};
        };
        s.exps.reserve(prog.exp_sources.size());
        for (const xva::BondSource &b : prog.exp_sources)
            s.exps.push_back(bond(b));
        s.coupons.reserve(prog.coupons.size());
        for (std::size_t j = 0; j < prog.coupons.size(); ++j)
        {
            const xva::CouponSource &c = prog.coupon_sources[j];
            const T g = m.G(c.start, c.end);
            s.coupons.push_back({T(c.beta * exp(m.log_discount(c.start) - m.log_discount(c.end) +
                                                0.5 * g * g * m.y(c.start))),
                                 g, prog.coupons[j].constant, prog.coupons[j].scale, prog.coupons[j].fixing});
        }
        s.accrued.reserve(prog.accrued.size());
        for (std::size_t j = 0; j < prog.accrued.size(); ++j)
        {
            const xva::BasicProgramExp<T> b = bond(prog.accrued_sources[j]);
            s.accrued.push_back({b.amount, b.G, prog.accrued[j].coupon});
        }
        s.option_dates.reserve(prog.option_dates.size());
        for (const xva::OptionDateSource &o : prog.option_date_sources)
            s.option_dates.push_back(program_option_date<T>(m, o.from, o.expiry));
        s.option_bonds.reserve(prog.option_bonds.size());
        for (const xva::OptionBondSource &b : prog.option_bond_sources)
            s.option_bonds.push_back(
                {T(b.raw * exp(m.log_discount(b.to) - m.log_discount(b.expiry))), m.G(b.expiry, b.to)});
    }

    /// What the trades of the program are worth today, quantities included.
    template <class T, class M>
    T program_value_today(const xva::ExposureProgram &prog, const ProgramSetup<T> &s, M &m)
    {
        using std::exp;
        const xva::BasicProgramView<T> view = s.view(prog);
        T value(0.0);
        for (std::size_t k = 0; k < prog.trades.size(); ++k)
        {
            const xva::TodaySource &today = prog.today[k];
            T v(0.0);
            if (today.expiry > 0.0)
            {
                const T x(0.0);
                v = xva::program_option_integral(view, prog.trades[k],
                                                 program_option_date<T>(m, 0.0, today.expiry), x);
            }
            else
                for (int j = today.term_begin; j < today.term_end; ++j)
                    v += prog.today_terms[static_cast<std::size_t>(j)].raw *
                         exp(m.log_discount(prog.today_terms[static_cast<std::size_t>(j)].to));
            value += prog.trades[k].quantity * v;
        }
        return value;
    }

    /// A tenor written for a risk label: "5Y", or "2.50Y" off whole years.
    inline std::string years(Time t)
    {
        char buffer[32];
        if (std::abs(t - std::round(t)) < 1e-9)
            std::snprintf(buffer, sizeof buffer, "%.0fY", t);
        else
            std::snprintf(buffer, sizeof buffer, "%.2fY", t);
        return buffer;
    }

    /// The default and funding weights of each reporting date (risk/xva.hpp:
    /// first to default, and the running funding cost), appended to
    /// `s.weights`. `F` holds the reporting plan, the time grid and the hazard
    /// times; `P` the hazards, losses given default and funding spreads in T.
    template <class T, class F, class P, class S>
    void push_default_and_funding_weights(const F &f, const P &p, S &s)
    {
        using std::exp;
        T sc_before(1.0), si_before(1.0);
        Time previous = 0.0;
        for (const int i : f.plan.reporting)
        {
            const Time t = f.times[static_cast<std::size_t>(i)];
            const T sc = exp(log_survival(f.hazard_times_c, p.hazard_c, t));
            const T si = exp(log_survival(f.hazard_times_i, p.hazard_i, t));
            const auto push = [&s](XvaOutput o, const T &w)
            { s.weights[static_cast<std::size_t>(o)].push_back(w); };
            push(XvaOutput::Cva, T(-p.lgd_c * si_before * (sc_before - sc)));
            push(XvaOutput::Dva, T(-p.lgd_i * sc_before * (si_before - si)));
            push(XvaOutput::Fca, T(-p.spread_b * sc * si * (t - previous)));
            push(XvaOutput::Fba, T(-p.spread_l * sc * si * (t - previous)));
            push(XvaOutput::CvaUnilateral, T(-p.lgd_c * (sc_before - sc)));
            sc_before = sc;
            si_before = si;
            previous = t;
        }
    }

    /// Adds the discounted exposure `v` of reporting date `r` to the outputs:
    /// a positive one to the CVA, the FCA and the unilateral CVA, a negative
    /// one to the DVA and the FBA (the order of XvaOutput).
    template <class T, class W, class Out>
    void accumulate_outputs(const T &v, std::size_t r, const W &weights, Out &out)
    {
        if (v > 0.0)
        {
            out[0] += weights[0][r] * v;
            out[2] += weights[2][r] * v;
            out[4] += weights[4][r] * v;
        }
        else if (v < 0.0)
        {
            out[1] += weights[1][r] * v;
            out[3] += weights[3][r] * v;
        }
    }

    /// Registers the credit and funding inputs as risk factors, through `add`
    /// (factor, tenor, label), and copies them into the parameters `p` and the
    /// hazard times of `f`.
    template <class Add, class F, class P>
    void add_credit_and_funding(const Add &add, const XvaRiskInputs &inputs, F &f, P &p)
    {
        const auto hazards = [&add](const CreditCurve &c, std::vector<Time> &times, std::vector<Real> &values,
                                    XvaRiskFactor factor, const char *who)
        {
            times = c.times();
            values = c.hazards();
            // One period is a flat curve, whatever date it is written to.
            const bool flat = values.size() == 1;
            for (std::size_t j = 0; j < values.size(); ++j)
                add(factor, flat || j >= times.size() ? 0.0 : times[j],
                    std::string(who) + " hazard rate" +
                        (flat ? std::string() : j < times.size() ? " to " + years(times[j])
                                                                 : " beyond"));
        };
        hazards(inputs.counterparty, f.hazard_times_c, p.hazard_c, XvaRiskFactor::CounterpartyHazard, "counterparty");
        hazards(inputs.own, f.hazard_times_i, p.hazard_i, XvaRiskFactor::OwnHazard, "own");
        p.lgd_c = inputs.lgd_counterparty;
        p.lgd_i = inputs.lgd_own;
        p.spread_b = inputs.borrowing_spread;
        p.spread_l = inputs.lending_spread;
        add(XvaRiskFactor::CounterpartyLgd, 0.0, "counterparty loss given default");
        add(XvaRiskFactor::OwnLgd, 0.0, "own loss given default");
        add(XvaRiskFactor::BorrowingSpread, 0.0, "borrowing spread");
        add(XvaRiskFactor::LendingSpread, 0.0, "lending spread");
    }

    /// The xVA values alone, in doubles: `evaluate(ws, path, out)` writes the
    /// outputs of one path, with a workspace `make_workspace()` builds once per
    /// batch. Batches of kBatch paths, spread over `pool`.
    template <class MakeWorkspace, class Evaluate>
    XvaValues xva_values_over_batches(std::size_t paths, ThreadPool *pool, const MakeWorkspace &make_workspace,
                                      const Evaluate &evaluate)
    {
        const std::size_t batches = (paths + kBatch - 1) / kBatch;
        std::vector<std::array<Real, kXvaOutputs>> means(batches);
        const auto started = std::chrono::steady_clock::now();
        for_each_batch(paths, pool,
                       [&](std::size_t b, std::size_t first, std::size_t size)
                       {
                           auto ws = make_workspace();
                           std::array<Real, kXvaOutputs> out{}, sum{};
                           for (std::size_t q = 0; q < size; ++q)
                           {
                               evaluate(ws, first + q, out);
                               for (std::size_t o = 0; o < kXvaOutputs; ++o)
                                   sum[o] += out[o];
                           }
                           for (std::size_t o = 0; o < kXvaOutputs; ++o)
                               means[b][o] = sum[o] / static_cast<Real>(size);
                       });
        XvaValues result;
        result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        result.paths = paths;
        result.batches = batches;
        for (std::size_t o = 0; o < kXvaOutputs; ++o)
        {
            std::vector<Real> column(batches);
            for (std::size_t b = 0; b < batches; ++b)
                column[b] = means[b][o];
            result.values[o] = over_batches(column, paths);
        }
        return result;
    }

} // namespace quantModeling::xva_adjoint

#endif // QM_SRC_ENGINES_XVA_ADJOINT_SUPPORT_HPP
