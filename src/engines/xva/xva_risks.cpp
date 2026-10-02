#include "quantModeling/engines/xva/exposure_engine.hpp"

#include "quantModeling/aad/number.hpp"
#include "quantModeling/aad/tape.hpp"
#include "quantModeling/risk/collateral.hpp"
#include "quantModeling/utils/accumulators.hpp"
#include "quantModeling/utils/philox.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <map>
#include <memory>
#include <numeric>
#include <string>

// Lot X8 of blueprint/wp/23-xva.md (§14.13): the adjoint of the exposure
// engine. One generic code, instantiated on the differentiable number for
// the risks and on double for the values they are checked against.

namespace quantModeling::aad
{
    /// A rounded amount does not move with what it rounds: a constant
    /// (risk/collateral_path.hpp finds this overload by its argument).
    inline Number round_to_multiple(const Number &x, double unit)
    {
        return Number(std::round(x.value() / unit) * unit);
    }
} // namespace quantModeling::aad

namespace quantModeling
{
    namespace
    {
        using aad::Number;

        constexpr std::size_t kBatch = 64;
        constexpr Real kTimeEps = 1e-10;

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

        /// The inputs the adjustments are differentiated to.
        template <class T>
        struct Params
        {
            std::vector<T> zero;
            T a, sigma;
            std::vector<T> hazard_c, hazard_i;
            T lgd_c, lgd_i, spread_b, spread_l;

            /// Every input in the order of XvaRisks::factors.
            std::vector<const T *> all() const
            {
                std::vector<const T *> out;
                for (const T &z : zero)
                    out.push_back(&z);
                out.push_back(&a);
                out.push_back(&sigma);
                for (const T &h : hazard_c)
                    out.push_back(&h);
                for (const T &h : hazard_i)
                    out.push_back(&h);
                for (const T *x : {&lgd_c, &lgd_i, &spread_b, &spread_l})
                    out.push_back(x);
                return out;
            }
        };

        /// What does not depend on the inputs.
        struct Fixed
        {
            xva::ExposureProgram program;
            std::vector<Time> times;
            /// The pillars of the curve; empty for a flat curve, whose one
            /// input is its rate.
            std::vector<Time> pillar_times;
            bool flat_forward = true;
            std::vector<Time> hazard_times_c, hazard_times_i;
            bool collateralised = false;
            /// Its `reporting` is always filled; the rest only under a CSA.
            CollateralPlan plan;
            std::vector<Real> widths;
            std::uint64_t seed = 0;

            /**
             * The trades that are not options, netted: at each date, the
             * cash flows of one maturity of all of them are one term,
             * (Σ quantity × amount) × exp(-G x), since G depends on the two
             * dates alone. A book of swaps pays on a few dozen dates, so the
             * netting set has a few dozen terms a date where its trades have
             * hundreds: that many fewer exponentials, and a tape that many
             * times shorter.
             */
            struct NettedTerm
            {
                /// The terms of the program that are netted, and their quantities.
                std::vector<std::pair<int, Real>> parts;
            };
            std::vector<NettedTerm> netted;
            /// netted_begin[i] .. netted_begin[i + 1]: the terms of date i.
            std::vector<int> netted_begin;
            /// The trades that are not options.
            std::vector<std::size_t> linear_trades;

            void net()
            {
                netted.clear();
                netted_begin.assign(1, 0);
                linear_trades.clear();
                for (std::size_t k = 0; k < program.trades.size(); ++k)
                    if (program.trades[k].expiry < 0)
                        linear_trades.push_back(k);
                for (std::size_t i = 0; i < times.size(); ++i)
                {
                    std::map<Time, std::size_t> by_maturity;
                    for (const std::size_t k : linear_trades)
                    {
                        const xva::ProgramTrade &trade = program.trades[k];
                        const xva::ProgramDate &date =
                            program.date_records[static_cast<std::size_t>(trade.date_begin) + i];
                        for (int j = date.exp_begin; j < date.exp_end; ++j)
                        {
                            const Time maturity = program.exp_sources[static_cast<std::size_t>(j)].to;
                            const auto [it, added] = by_maturity.emplace(maturity, netted.size());
                            if (added)
                                netted.emplace_back();
                            netted[it->second].parts.emplace_back(j, trade.quantity);
                        }
                    }
                    netted_begin.push_back(static_cast<int>(netted.size()));
                }
            }
        };

        /// The curve and the model as functions of the inputs: the formulas of
        /// DiscountCurve::discount and HullWhiteCurveModel, in T.
        template <class T>
        class Model
        {
          public:
            Model(const Fixed &fixed, const Params<T> &params)
                : f_(fixed), p_(params)
            {
                for (std::size_t k = 0; k < f_.pillar_times.size(); ++k)
                    log_df_.push_back(T(-p_.zero[k] * f_.pillar_times[k]));
            }

            /// ln P(0, t).
            T log_discount(Time t)
            {
                if (t <= 0.0)
                    return T(0.0);
                const auto found = cache_.find(t);
                if (found != cache_.end())
                    return found->second;
                const T l = compute_log_discount(t);
                cache_.emplace(t, l);
                return l;
            }

            T G(Time t, Time maturity) const
            {
                return T(-expm1_of(T(-p_.a * (maturity - t))) / p_.a);
            }

            T y(Time t) const
            {
                return T(p_.sigma * p_.sigma * (-expm1_of(T(-2.0 * p_.a * t))) / (2.0 * p_.a));
            }

            struct Transition
            {
                T decay, drift, variance;
            };

            Transition transition(Time s, Time t, Time numeraire_maturity) const
            {
                using std::exp;
                const T &a = p_.a;
                const T s2 = p_.sigma * p_.sigma;
                const Time d = t - s;
                const T em = expm1_of(T(-a * d));
                const T ep = expm1_of(T(a * d));
                const T e2 = expm1_of(T(-2.0 * a * d));
                const T I1 = s2 / (2.0 * a * a) * (-em - exp(-2.0 * a * t) * ep);
                const T I2 = (-em + 0.5 * exp(-a * (numeraire_maturity - t)) * e2) / (a * a);
                return {T(exp(-a * d)), T(I1 - s2 * I2), T(s2 * (-e2) / (2.0 * a))};
            }

          private:
            T compute_log_discount(Time t) const
            {
                const std::vector<Time> &ts = f_.pillar_times;
                if (ts.empty())
                    return T(-p_.zero[0] * t);
                const std::size_t n = ts.size();
                if (t <= ts.front())
                    return f_.flat_forward ? T(log_df_[0] * (t / ts.front())) : log_df_[0];
                if (t >= ts.back())
                {
                    if (!f_.flat_forward)
                        return log_df_[n - 1];
                    if (n == 1)
                        return T(log_df_[0] + log_df_[0] / ts[0] * (t - ts.back()));
                    return T(log_df_[n - 1] + (log_df_[n - 1] - log_df_[n - 2]) / (ts[n - 1] - ts[n - 2]) *
                                                  (t - ts.back()));
                }
                const std::size_t idx =
                    static_cast<std::size_t>(std::upper_bound(ts.begin(), ts.end(), t) - ts.begin());
                const Real w = (t - ts[idx - 1]) / (ts[idx] - ts[idx - 1]);
                return T((1.0 - w) * log_df_[idx - 1] + w * log_df_[idx]);
            }

            const Fixed &f_;
            const Params<T> &p_;
            std::vector<T> log_df_;
            std::map<Time, T> cache_;
        };

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

        /// Everything the paths read, as functions of the inputs: recorded
        /// before the mark in a differentiable run.
        template <class T>
        struct Setup
        {
            std::vector<xva::BasicProgramExp<T>> exps;
            std::vector<xva::BasicProgramCoupon<T>> coupons;
            std::vector<xva::BasicProgramAccrued<T>> accrued;
            std::vector<xva::BasicProgramOptionDate<T>> option_dates;
            std::vector<xva::BasicProgramOptionBond<T>> option_bonds;
            xva::BasicStateDynamics<T> dynamics;
            /// The netted terms of Fixed::netted, as amount × exp(-G x).
            std::vector<xva::BasicProgramExp<T>> netted;
            /// Per output, the weight of each reporting date.
            std::array<std::vector<T>, kXvaOutputs> weights;
            T held_today;

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

        template <class T>
        xva::BasicProgramOptionDate<T> option_date(Model<T> &m, Time from, Time expiry)
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

        template <class T>
        void build(const Fixed &f, const Params<T> &p, Setup<T> &s)
        {
            using std::exp;
            using std::sqrt;
            const xva::ExposureProgram &prog = f.program;
            Model<T> m(f, p);
            // y(t) is asked for at every grid date, many times over.
            std::map<Time, T> y_cache;
            const auto y = [&](Time t)
            {
                const auto found = y_cache.find(t);
                if (found != y_cache.end())
                    return found->second;
                const T v = m.y(t);
                y_cache.emplace(t, v);
                return v;
            };
            const auto bond = [&](const xva::BondSource &b)
            {
                const T G = m.G(b.from, b.to);
                return xva::BasicProgramExp<T>{
                    T(b.raw * exp(m.log_discount(b.to) - m.log_discount(b.from) - 0.5 * G * G * y(b.from))), G};
            };

            // The terms of the options are kept one by one (an option is its
            // underlying only on the paths that exercise); those of the other
            // trades are only ever read netted, below.
            s.exps.resize(prog.exps.size());
            for (const xva::ProgramTrade &trade : prog.trades)
            {
                if (trade.expiry < 0)
                    continue;
                for (std::size_t i = 0; i < f.times.size(); ++i)
                {
                    const xva::ProgramDate &date = prog.date_records[static_cast<std::size_t>(trade.date_begin) + i];
                    for (int j = date.exp_begin; j < date.exp_end; ++j)
                        s.exps[static_cast<std::size_t>(j)] = bond(prog.exp_sources[static_cast<std::size_t>(j)]);
                }
            }
            s.netted.reserve(f.netted.size());
            for (const Fixed::NettedTerm &term : f.netted)
            {
                // Its parts are cash flows of one date seen from one date:
                // they share their discounting, and add up before it.
                xva::BondSource net = prog.exp_sources[static_cast<std::size_t>(term.parts.front().first)];
                net.raw = 0.0;
                for (const auto &[j, quantity] : term.parts)
                    net.raw += quantity * prog.exp_sources[static_cast<std::size_t>(j)].raw;
                s.netted.push_back(bond(net));
            }
            s.coupons.reserve(prog.coupons.size());
            for (std::size_t j = 0; j < prog.coupons.size(); ++j)
            {
                const xva::CouponSource &c = prog.coupon_sources[j];
                const T g = m.G(c.start, c.end);
                s.coupons.push_back({T(c.beta * exp(m.log_discount(c.start) - m.log_discount(c.end) +
                                                    0.5 * g * g * y(c.start))),
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
                s.option_dates.push_back(option_date(m, o.from, o.expiry));
            s.option_bonds.reserve(prog.option_bonds.size());
            for (const xva::OptionBondSource &b : prog.option_bond_sources)
                s.option_bonds.push_back(
                    {T(b.raw * exp(m.log_discount(b.to) - m.log_discount(b.expiry))), m.G(b.expiry, b.to)});

            // The state under the T*-forward measure, and the discount weight
            // P(0, t) exp(G x + G² y / 2), G = G(t, T*).
            const std::size_t n = f.times.size();
            const Time horizon = f.times.back();
            Time previous = 0.0;
            for (std::size_t i = 0; i < n; ++i)
            {
                const Time t = f.times[i];
                const auto tr = m.transition(previous, t, horizon);
                const T G = m.G(t, horizon);
                s.dynamics.decay.push_back(tr.decay);
                s.dynamics.drift.push_back(tr.drift);
                s.dynamics.sd.push_back(T(sqrt(tr.variance)));
                s.dynamics.weight_scale.push_back(T(exp(m.log_discount(t) + 0.5 * G * G * y(t))));
                s.dynamics.weight_G.push_back(G);
                previous = t;
            }

            // Default and funding weights of each reporting date
            // (risk/xva.hpp: first to default, and the running funding cost).
            T sc_before(1.0), si_before(1.0);
            previous = 0.0;
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

            // The collateral held today: the call on today's value.
            s.held_today = T(0.0);
            if (f.collateralised)
            {
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
                                                         option_date(m, 0.0, today.expiry), x);
                    }
                    else
                        for (int j = today.term_begin; j < today.term_end; ++j)
                            v += prog.today_terms[static_cast<std::size_t>(j)].raw *
                                 exp(m.log_discount(prog.today_terms[static_cast<std::size_t>(j)].to));
                    value += prog.trades[k].quantity * v;
                }
                s.held_today =
                    balance_after_call(T(0.0), variation_margin_required(value, f.plan.terms), f.plan.terms);
            }
        }

        CollateralPlanView view_of(const Fixed &f)
        {
            CollateralPlanView v = view(f.plan);
            v.dates = static_cast<int>(f.times.size());
            return v;
        }

        template <class T>
        struct Workspace
        {
            std::vector<T> state, value, flow, held;
            explicit Workspace(std::size_t n)
                : state(n), value(n), flow(n), held(n) {}
        };

        /// One path: its state, the value of the netting set at every date,
        /// the collateral, and its own CVA, DVA, FCA, FBA and unilateral CVA.
        template <class T>
        void evaluate_path(const Fixed &f, const Setup<T> &s, std::uint64_t path, Workspace<T> &ws,
                           std::array<T, kXvaOutputs> &out)
        {
            using std::exp;
            const xva::ExposureProgram &prog = f.program;
            const xva::BasicProgramView<T> view = s.view(prog);
            const int n = static_cast<int>(f.times.size());
            const bool flows = f.collateralised && f.plan.needs_cashflows();

            // The draws of the pricing engine: Φ⁻¹(Philox(seed, path, i)).
            PhiloxGaussianSource gaussian(f.seed, path);
            for (int i = 0; i < n; ++i)
            {
                const Real z = gaussian.next();
                if (i == 0)
                    ws.state[0] = s.dynamics.drift[0] + s.dynamics.sd[0] * z;
                else
                    ws.state[i] = s.dynamics.decay[i] * ws.state[i - 1] + s.dynamics.drift[i] + s.dynamics.sd[i] * z;
                ws.value[i] = T(0.0);
                if (flows)
                    ws.flow[i] = T(0.0);
            }
            const T *state = ws.state.data();

            // The trades that are not options, netted: a few terms a date,
            // four of them to a node of the tape.
            for (int i = 0; i < n; ++i)
            {
                const xva::BasicProgramExp<T> *t = s.netted.data() + f.netted_begin[static_cast<std::size_t>(i)];
                const xva::BasicProgramExp<T> *const end =
                    s.netted.data() + f.netted_begin[static_cast<std::size_t>(i) + 1];
                const T &x = state[i];
                for (; t + 4 <= end; t += 4)
                    ws.value[i] += t[0].amount * exp(-t[0].G * x) + t[1].amount * exp(-t[1].G * x) +
                                   t[2].amount * exp(-t[2].G * x) + t[3].amount * exp(-t[3].G * x);
                for (; t < end; ++t)
                    ws.value[i] += t->amount * exp(-t->G * x);
            }
            for (const std::size_t k : f.linear_trades)
            {
                // What is left of them: the coupons fixed and not yet paid,
                // and their cash flows.
                const xva::ProgramTrade &trade = prog.trades[k];
                const Real q = trade.quantity;
                for (int i = 0; i < n; ++i)
                {
                    const xva::ProgramDate &date = prog.date_records[static_cast<std::size_t>(trade.date_begin + i)];
                    for (int j = date.accrued_begin; j < date.accrued_end; ++j)
                        ws.value[i] += q * xva::program_coupon(view.coupons[view.accrued[j].coupon], state) *
                                       view.accrued[j].amount * exp(-view.accrued[j].G * state[i]);
                    if (flows)
                        ws.flow[i] += q * xva::program_linear_cashflow(view, trade, i, state);
                }
            }

            for (std::size_t k = 0; k < prog.trades.size(); ++k)
            {
                const xva::ProgramTrade &trade = prog.trades[k];
                const Real q = trade.quantity;
                const int e = trade.expiry;
                if (e < 0)
                    continue;
                for (int i = 0; i < e; ++i)
                    ws.value[i] += q * xva::program_option_value(view, trade, i, state);
                // At the expiry the option becomes its underlying or nothing.
                // The indicator is a ramp over [-width, width] of the
                // underlying's value: outside it nothing changes.
                const T at_expiry = xva::program_linear_value(view, trade, e, state);
                const Real width = f.widths[k];
                const Real v = value_of(at_expiry);
                if (width > 0.0 ? v <= -width : !(v > 0.0))
                    continue;
                if (!(width > 0.0) || v >= width)
                {
                    ws.value[e] += q * at_expiry;
                    for (int i = e + 1; i < n; ++i)
                    {
                        ws.value[i] += q * xva::program_linear_value(view, trade, i, state);
                        if (flows)
                            ws.flow[i] += q * xva::program_linear_cashflow(view, trade, i, state);
                    }
                    continue;
                }
                const T gate = 0.5 + at_expiry / (2.0 * width);
                ws.value[e] += q * gate * at_expiry;
                for (int i = e + 1; i < n; ++i)
                {
                    ws.value[i] += q * gate * xva::program_linear_value(view, trade, i, state);
                    if (flows)
                        ws.flow[i] += q * gate * xva::program_linear_cashflow(view, trade, i, state);
                }
            }

            const CollateralPlanView plan = view_of(f);
            if (f.collateralised)
                collateral_balances(plan, ws.value.data(), ws.held.data(), s.held_today);

            for (T &o : out)
                o = T(0.0);
            const std::size_t m = f.plan.reporting.size();
            for (std::size_t r = 0; r < m; ++r)
            {
                const int i = f.plan.reporting[r];
                const T v = s.dynamics.weight_scale[i] * exp(s.dynamics.weight_G[i] * ws.state[i]) *
                            (f.collateralised
                                 ? collateralised_value(plan, static_cast<int>(r), ws.value.data(), ws.flow.data(),
                                                        ws.held.data(), s.held_today)
                                 : ws.value[i]);
                if (v > 0.0)
                {
                    out[0] += s.weights[0][r] * v;
                    out[2] += s.weights[2][r] * v;
                    out[4] += s.weights[4][r] * v;
                }
                else if (v < 0.0)
                {
                    out[1] += s.weights[1][r] * v;
                    out[3] += s.weights[3][r] * v;
                }
            }
        }

        std::string years(Time t)
        {
            char buffer[32];
            if (std::abs(t - std::round(t)) < 1e-9)
                std::snprintf(buffer, sizeof buffer, "%.0fY", t);
            else
                std::snprintf(buffer, sizeof buffer, "%.2fY", t);
            return buffer;
        }

        /// The mean over the paths and its standard error, from the means of
        /// the batches, in batch order. The last batch may be short: it
        /// weighs its paths in the mean; the error is the dispersion of the
        /// batch means.
        Estimate over_batches(const std::vector<Real> &batch_means, std::size_t paths)
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
    } // namespace

    struct HullWhiteExposureEngine::RiskRun
    {
        Fixed fixed;
        Params<Real> params;
        std::vector<XvaRiskFactor> factors;
        std::vector<Time> tenors;
        std::vector<std::string> labels;
    };

    void HullWhiteExposureEngine::prepare_risk_run(ExposureSimulationSettings &settings,
                                                   const XvaRiskInputs &inputs, RiskRun &run)
    {
        if (trades_.empty())
            throw InvalidInput("exposure engine: no trade");
        if (settings.historical)
            throw InvalidInput("xVA risks: adjustments are prices and need the pricing measure");
        if (!(inputs.lgd_counterparty >= 0.0 && inputs.lgd_counterparty <= 1.0) ||
            !(inputs.lgd_own >= 0.0 && inputs.lgd_own <= 1.0))
            throw InvalidInput("xVA risks: loss given default must be in [0, 1]");
        if (!std::isfinite(inputs.borrowing_spread) || !std::isfinite(inputs.lending_spread))
            throw InvalidInput("xVA risks: the funding spreads must be finite");
        if (!(inputs.exercise_smoothing >= 0.0))
            throw InvalidInput("xVA risks: the exercise smoothing must be >= 0");
        const DiscountCurve &curve = model_.discount();
        const DiscountCurve &projection = model_.projection();
        if (curve.pillar_times() != projection.pillar_times() ||
            curve.pillar_discount_factors() != projection.pillar_discount_factors() ||
            curve.discount(1.0) != projection.discount(1.0))
            throw InvalidInput("xVA risks: the sensitivities are computed under one curve; this "
                               "model has a projection curve of its own");
        if (inputs.csa)
        {
            inputs.csa->validate();
            settings.grid.margin_period_of_risk = inputs.csa->margin_period_of_risk;
        }

        Fixed &f = run.fixed;
        ExposurePaths head;
        prepare(settings, head);
        f.times = head.times;
        f.seed = settings.seed;
        Real value_today = 0.0;
        for (std::size_t k = 0; k < trades_.size(); ++k)
        {
            if (!trades_[k].value->compile(f.program))
                throw InvalidInput("xVA risks: trade " + std::to_string(k) +
                                   " is valued by regression; its sensitivities are not computed");
            f.program.trades.back().quantity = trades_[k].quantity;
            value_today += trades_[k].quantity * trades_[k].value->value_today();
        }

        f.net();

        f.collateralised = inputs.csa.has_value();
        if (inputs.csa)
            f.plan = collateral_plan(f.times, *inputs.csa, value_today, inputs.cashflows);
        else
        {
            f.plan.reporting.resize(f.times.size());
            std::iota(f.plan.reporting.begin(), f.plan.reporting.end(), 0);
        }

        // The inputs, in the order they are reported in.
        Params<Real> &p = run.params;
        const auto add = [&run](XvaRiskFactor factor, Time tenor, std::string label)
        {
            run.factors.push_back(factor);
            run.tenors.push_back(tenor);
            run.labels.push_back(std::move(label));
        };
        f.pillar_times = curve.pillar_times();
        f.flat_forward = curve.extrapolation() == CurveExtrapolation::FlatForward;
        if (f.pillar_times.empty())
        {
            p.zero.push_back(-std::log(curve.discount(1.0)));
            add(XvaRiskFactor::ZeroRate, 0.0, "zero rate, flat curve");
        }
        else
            for (std::size_t k = 0; k < f.pillar_times.size(); ++k)
            {
                p.zero.push_back(-std::log(curve.pillar_discount_factors()[k]) / f.pillar_times[k]);
                add(XvaRiskFactor::ZeroRate, f.pillar_times[k], "zero rate " + years(f.pillar_times[k]));
            }
        p.a = model_.mean_reversion();
        p.sigma = model_.sigma();
        add(XvaRiskFactor::MeanReversion, 0.0, "Hull-White mean reversion");
        add(XvaRiskFactor::Sigma, 0.0, "Hull-White volatility");
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
        hazards(inputs.counterparty, f.hazard_times_c, p.hazard_c, XvaRiskFactor::CounterpartyHazard,
                "counterparty");
        hazards(inputs.own, f.hazard_times_i, p.hazard_i, XvaRiskFactor::OwnHazard, "own");
        p.lgd_c = inputs.lgd_counterparty;
        p.lgd_i = inputs.lgd_own;
        p.spread_b = inputs.borrowing_spread;
        p.spread_l = inputs.lending_spread;
        add(XvaRiskFactor::CounterpartyLgd, 0.0, "counterparty loss given default");
        add(XvaRiskFactor::OwnLgd, 0.0, "own loss given default");
        add(XvaRiskFactor::BorrowingSpread, 0.0, "borrowing spread");
        add(XvaRiskFactor::LendingSpread, 0.0, "lending spread");

        // The ramp of each option: a fraction of the standard deviation of
        // its underlying's value at the expiry, |dV/dx| sd(x) at the mean
        // state.
        const std::size_t K = f.program.trades.size();
        if (!inputs.smoothing_widths.empty())
        {
            if (inputs.smoothing_widths.size() != K)
                throw InvalidInput("xVA risks: one smoothing width per trade is needed");
            f.widths = inputs.smoothing_widths;
            return;
        }
        f.widths.assign(K, 0.0);
        if (!(inputs.exercise_smoothing > 0.0))
            return;
        Setup<Real> setup;
        build(f, p, setup);
        std::vector<Real> mean(f.times.size()), sd(f.times.size());
        Real mu = 0.0, variance = 0.0;
        for (std::size_t i = 0; i < f.times.size(); ++i)
        {
            mu = setup.dynamics.decay[i] * mu + setup.dynamics.drift[i];
            variance = setup.dynamics.decay[i] * setup.dynamics.decay[i] * variance +
                       setup.dynamics.sd[i] * setup.dynamics.sd[i];
            mean[i] = mu;
            sd[i] = std::sqrt(variance);
        }
        for (std::size_t k = 0; k < K; ++k)
        {
            const xva::ProgramTrade &trade = f.program.trades[k];
            if (trade.expiry < 0)
                continue;
            const std::size_t e = static_cast<std::size_t>(trade.expiry);
            const xva::ProgramDate &date = f.program.date_records[static_cast<std::size_t>(trade.date_begin) + e];
            Real slope = 0.0;
            for (int j = date.exp_begin; j < date.exp_end; ++j)
            {
                const auto &b = setup.exps[static_cast<std::size_t>(j)];
                slope -= b.G * b.amount * std::exp(-b.G * mean[e]);
            }
            f.widths[k] = inputs.exercise_smoothing * std::abs(slope) * sd[e];
        }
    }

    XvaValues HullWhiteExposureEngine::xva_values(ExposureSimulationSettings settings,
                                                  const XvaRiskInputs &inputs, ThreadPool *pool)
    {
        RiskRun run;
        prepare_risk_run(settings, inputs, run);
        const Fixed &f = run.fixed;
        Setup<Real> setup;
        build(f, run.params, setup);

        const std::size_t N = settings.paths;
        const std::size_t batches = (N + kBatch - 1) / kBatch;
        std::vector<std::array<Real, kXvaOutputs>> means(batches);
        const auto started = std::chrono::steady_clock::now();
        for_each_batch(N, pool,
                       [&](std::size_t b, std::size_t first, std::size_t size)
                       {
                           Workspace<Real> ws(f.times.size());
                           std::array<Real, kXvaOutputs> out{}, sum{};
                           for (std::size_t p = 0; p < size; ++p)
                           {
                               evaluate_path(f, setup, first + p, ws, out);
                               for (std::size_t o = 0; o < kXvaOutputs; ++o)
                                   sum[o] += out[o];
                           }
                           for (std::size_t o = 0; o < kXvaOutputs; ++o)
                               means[b][o] = sum[o] / static_cast<Real>(size);
                       });
        XvaValues result;
        result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        result.paths = N;
        result.batches = batches;
        result.smoothing_widths = f.widths;
        for (std::size_t o = 0; o < kXvaOutputs; ++o)
        {
            std::vector<Real> column(batches);
            for (std::size_t b = 0; b < batches; ++b)
                column[b] = means[b][o];
            result.values[o] = over_batches(column, N);
        }
        return result;
    }

    XvaRisks HullWhiteExposureEngine::xva_risks(ExposureSimulationSettings settings,
                                                const XvaRiskInputs &inputs, ThreadPool *pool)
    {
        RiskRun run;
        prepare_risk_run(settings, inputs, run);
        const Fixed &f = run.fixed;
        const std::size_t N = settings.paths;
        const std::size_t batches = (N + kBatch - 1) / kBatch;
        const std::size_t P = run.factors.size();

        // One tape per thread, with the set-up recorded on it once.
        struct ThreadState
        {
            aad::Tape tape;
            Params<Number> params;
            Setup<Number> setup;
            std::unique_ptr<Workspace<Number>> workspace;
            bool initialised = false;
        };
        const std::size_t threads = (pool != nullptr ? pool->num_threads() : 0) + 1;
        std::vector<std::unique_ptr<ThreadState>> states(threads);
        for (auto &state : states)
            state = std::make_unique<ThreadState>();

        std::vector<std::array<Real, kXvaOutputs>> means(batches);
        std::vector<Real> batch_risks(batches * kXvaOutputs * P, 0.0);
        const auto started = std::chrono::steady_clock::now();
        for_each_batch(
            N, pool,
            [&](std::size_t b, std::size_t first, std::size_t size)
            {
                ThreadState &st = *states[ThreadPool::thread_num()];
                const TapeScope scope(&st.tape);
                if (!st.initialised)
                {
                    st.tape.rewind();
                    // The inputs are the leaves; the set-up is recorded once,
                    // before the mark.
                    const Params<Real> &p = run.params;
                    for (const Real z : p.zero)
                        st.params.zero.emplace_back(z);
                    st.params.a = Number(p.a);
                    st.params.sigma = Number(p.sigma);
                    for (const Real h : p.hazard_c)
                        st.params.hazard_c.emplace_back(h);
                    for (const Real h : p.hazard_i)
                        st.params.hazard_i.emplace_back(h);
                    st.params.lgd_c = Number(p.lgd_c);
                    st.params.lgd_i = Number(p.lgd_i);
                    st.params.spread_b = Number(p.spread_b);
                    st.params.spread_l = Number(p.spread_l);
                    build(f, st.params, st.setup);
                    st.tape.mark();
                    st.workspace = std::make_unique<Workspace<Number>>(f.times.size());
                    st.initialised = true;
                }
                std::array<Number, kXvaOutputs> out;
                std::array<Real, kXvaOutputs> sum{};
                for (std::size_t p = 0; p < size; ++p)
                {
                    st.tape.rewind_to_mark();
                    evaluate_path(f, st.setup, first + p, *st.workspace, out);
                    for (std::size_t o = 0; o < kXvaOutputs; ++o)
                    {
                        sum[o] += out[o].value();
                        out[o].adjoint(o) = 1.0; // this output's own component
                    }
                    Number::propagate_to_mark_multi();
                }
                // What accumulated at the mark goes down to the inputs: the
                // risks of this batch.
                Number::propagate_mark_to_start_multi();
                const std::vector<const Number *> leaves = st.params.all();
                for (std::size_t o = 0; o < kXvaOutputs; ++o)
                {
                    means[b][o] = sum[o] / static_cast<Real>(size);
                    for (std::size_t j = 0; j < P; ++j)
                        batch_risks[(b * kXvaOutputs + o) * P + j] =
                            const_cast<Number *>(leaves[j])->adjoint(o) / static_cast<Real>(size);
                }
                st.tape.reset_adjoints_before_mark();
            });

        XvaRisks result;
        result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        result.paths = N;
        result.batches = batches;
        result.smoothing_widths = f.widths;
        result.factors = run.factors;
        result.tenors = run.tenors;
        result.labels = run.labels;
        for (const Real *x : run.params.all())
            result.levels.push_back(*x);
        result.risks.resize(kXvaOutputs * P);
        result.errors.resize(kXvaOutputs * P);
        std::vector<Real> column(batches);
        for (std::size_t o = 0; o < kXvaOutputs; ++o)
        {
            for (std::size_t b = 0; b < batches; ++b)
                column[b] = means[b][o];
            result.values[o] = over_batches(column, N);
            for (std::size_t j = 0; j < P; ++j)
            {
                for (std::size_t b = 0; b < batches; ++b)
                    column[b] = batch_risks[(b * kXvaOutputs + o) * P + j];
                const Estimate e = over_batches(column, N);
                result.risks[o * P + j] = e.value;
                result.errors[o * P + j] = e.error;
            }
        }
        result.batch_risks = std::move(batch_risks);
        return result;
    }

} // namespace quantModeling
