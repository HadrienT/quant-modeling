#include "quantModeling/engines/xva/cross_currency_exposure_engine.hpp"

#include "adjoint_support.hpp"

#include "quantModeling/utils/gauss_legendre.hpp"
#include "quantModeling/utils/philox.hpp"
#include "quantModeling/utils/stats.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <string>

// blueprint/wp/23-xva.md §14.17: the adjoint of the two-currency exposure
// engine — two Hull-White rates and a lognormal exchange rate, or one rate
// and an asset. The structure is that of the one-rate adjoint
// (xva_risks.cpp, lot X8): one generic code, instantiated on the
// differentiable number for the risks and on double for the values they are
// checked against; the set-up recorded once before the mark, the paths after
// it, the adjoints of a batch carried down to the inputs.

namespace quantModeling
{
    namespace
    {
        using aad::Number;
        using namespace xva_adjoint;

        constexpr Real kTimeEps = 1e-10;
        /// The streams of the engine's draws (cross_currency_exposure_engine.cpp).
        constexpr std::uint64_t kForeignStream = 0x9E3779B97F4A7C15ULL;
        constexpr std::uint64_t kFxStream = 0xC2B2AE3D27D4EB4FULL;

        inline double cdf_of(double x)
        {
            return norm_cdf(x);
        }
        inline Number cdf_of(const Number &x)
        {
            return Number(aad::normal_cdf(x));
        }

        /// The inputs the adjustments are differentiated to.
        template <class T>
        struct Params
        {
            std::vector<T> zero_d, zero_f;
            T a_d, sigma_d, a_f, sigma_f;
            T spot, sigma_s;
            T rho_df, rho_ds, rho_fs;
            std::vector<T> hazard_c, hazard_i;
            T lgd_c, lgd_i, spread_b, spread_l;

            /// Every input in the order of XvaRisks::factors.
            std::vector<const T *> all() const
            {
                std::vector<const T *> out;
                for (const T &z : zero_d)
                    out.push_back(&z);
                for (const T &z : zero_f)
                    out.push_back(&z);
                for (const T *x : {&a_d, &sigma_d, &a_f, &sigma_f, &spot, &sigma_s, &rho_df, &rho_ds, &rho_fs})
                    out.push_back(x);
                for (const T &h : hazard_c)
                    out.push_back(&h);
                for (const T &h : hazard_i)
                    out.push_back(&h);
                for (const T *x : {&lgd_c, &lgd_i, &spread_b, &spread_l})
                    out.push_back(x);
                return out;
            }
        };

        /// A curve given by the zero rates of its pillars.
        struct CurveShape
        {
            std::vector<Time> pillar_times;
            bool flat_forward = true;
        };

        /// What does not depend on the inputs.
        struct Fixed
        {
            std::vector<Time> times;
            CurveShape domestic, foreign;
            std::vector<Time> hazard_times_c, hazard_times_i;
            std::uint64_t seed = 0;

            /// The cash flows of the netting set, quantities included, one
            /// per payment date and currency: flows of one date share their
            /// discounting and add up before it.
            struct Flow
            {
                Time time;
                Real amount;
            };
            std::vector<Flow> flows_d, flows_f;
            /// first_d[i] .. flows_d.size(): the flows strictly after date i
            /// (they are sorted by time).
            std::vector<std::size_t> first_d, first_f;

            struct Option
            {
                Real strike;
                Time expiry;
                bool call;
                /// Units × quantity: negative for a sold option.
                Real notional;
                std::size_t expiry_index;
            };
            std::vector<Option> options;
        };

        /// ln P(0, t) of one of the two curves, in T, kept by date.
        template <class T>
        class Curve
        {
          public:
            Curve(const CurveShape &shape, const std::vector<T> &zero)
                : shape_(shape), zero_(zero)
            {
                for (std::size_t k = 0; k < shape_.pillar_times.size(); ++k)
                    log_df_.push_back(T(-zero_[k] * shape_.pillar_times[k]));
            }

            T log_discount(Time t)
            {
                if (t <= 0.0)
                    return T(0.0);
                const auto found = cache_.find(t);
                if (found != cache_.end())
                    return found->second;
                const T l = curve_log_discount(shape_.pillar_times, shape_.flat_forward, zero_, log_df_, t);
                cache_.emplace(t, l);
                return l;
            }

          private:
            const CurveShape &shape_;
            const std::vector<T> &zero_;
            std::vector<T> log_df_;
            std::map<Time, T> cache_;
        };

        /// CrossCurrencyHullWhiteModel, in T: the same formulas, so that the
        /// run on doubles is the engine's own simulation.
        template <class T>
        class Model
        {
          public:
            Model(const Fixed &fixed, const Params<T> &params)
                : p_(params), domestic(fixed.domestic, params.zero_d), foreign(fixed.foreign, params.zero_f)
            {
            }

            static T G(const T &a, Time t, Time maturity)
            {
                return T(-expm1_of(T(-a * (maturity - t))) / a);
            }
            static T y(const T &a, const T &sigma, Time t)
            {
                return T(sigma * sigma * (-expm1_of(T(-2.0 * a * t))) / (2.0 * a));
            }
            T G_d(Time t, Time maturity) const { return G(p_.a_d, t, maturity); }
            T G_f(Time t, Time maturity) const { return G(p_.a_f, t, maturity); }
            T y_d(Time t) const { return y(p_.a_d, p_.sigma_d, t); }
            T y_f(Time t) const { return y(p_.a_f, p_.sigma_f, t); }

            /// σ_F(u)² of the forward to T: the squared norm of its volatility.
            T forward_variance_rate(Time u, Time T_) const
            {
                const T &s = p_.sigma_s;
                const T d = p_.sigma_d * G_d(u, T_);
                const T f = p_.sigma_f * G_f(u, T_);
                return T(s * s + d * d + f * f + 2.0 * p_.rho_ds * s * d - 2.0 * p_.rho_fs * s * f -
                         2.0 * p_.rho_df * d * f);
            }

            /// ∫_t^T σ_F(u; T)² du, in pieces of at most a year.
            T forward_variance(Time t, Time T_) const
            {
                if (T_ - t < 1e-14)
                    return T(0.0);
                const int pieces = static_cast<int>(std::ceil(T_ - t));
                T variance(0.0);
                for (int k = 0; k < pieces; ++k)
                    variance = variance + gauss_legendre(t + (T_ - t) * k / pieces, t + (T_ - t) * (k + 1) / pieces,
                                                         [this, T_](Time u)
                                                         { return forward_variance_rate(u, T_); });
                return variance;
            }

            struct Transition
            {
                std::array<T, 2> decay;
                std::array<T, 3> drift;
                /// l00, l10, l11, l20, l21, l22.
                std::array<T, 6> cholesky;
            };

            /// The exact Gaussian step of (x_d, x_f, z) from s to t under the
            /// domestic forward measure of `horizon`.
            Transition transition(Time s, Time t, Time horizon) const
            {
                using std::exp;
                using std::sqrt;
                const T &ad = p_.a_d, &af = p_.a_f, &sd = p_.sigma_d, &sf = p_.sigma_f, &ss = p_.sigma_s;
                const T &rdf = p_.rho_df, &rds = p_.rho_ds, &rfs = p_.rho_fs;
                const HullWhiteTransition<T> dom = hull_white_transition(ad, sd, s, t, horizon);
                const Time d = t - s;

                Transition out;
                out.decay = {dom.decay, T(exp(-af * d))};
                out.drift[0] = dom.drift;
                out.drift[1] = gauss_legendre(s, t,
                                              [&](Time u)
                                              {
                                                  return T(exp(-af * (t - u)) *
                                                           (y_f(u) - rfs * sf * ss - rdf * sf * sd * G_d(u, horizon)));
                                              });
                const T variance_z = gauss_legendre(s, t,
                                                    [&](Time u)
                                                    { return forward_variance_rate(u, horizon); });
                out.drift[2] = T(-0.5 * variance_z);

                const T c00 = dom.variance;
                const T c11 = sf * sf * (-expm1_of(T(-2.0 * af * d))) / (2.0 * af);
                const T sum = ad + af;
                const T c01 = rdf * sd * sf * (-expm1_of(T(-sum * d))) / sum;
                const T c02 = gauss_legendre(s, t,
                                             [&](Time u)
                                             {
                                                 return T(exp(-ad * (t - u)) * sd *
                                                          (rds * ss - rdf * sf * G_f(u, horizon) +
                                                           sd * G_d(u, horizon)));
                                             });
                const T c12 = gauss_legendre(s, t,
                                             [&](Time u)
                                             {
                                                 return T(exp(-af * (t - u)) * sf *
                                                          (rfs * ss - sf * G_f(u, horizon) +
                                                           rdf * sd * G_d(u, horizon)));
                                             });
                const T l00 = sqrt(c00);
                const T l10 = c01 / l00, l20 = c02 / l00;
                const T l11 = sqrt(c11 - l10 * l10);
                const T l21 = (c12 - l20 * l10) / l11;
                const T l22 = sqrt(variance_z - l20 * l20 - l21 * l21);
                if (!std::isfinite(value_of(l11)) || !std::isfinite(value_of(l22)))
                    throw InvalidInput("two-currency xVA risks: the covariance of a step is not positive "
                                       "definite");
                out.cholesky = {l00, l10, l11, l20, l21, l22};
                return out;
            }

          private:
            const Params<T> &p_;

          public:
            Curve<T> domestic, foreign;
        };

        /// amount × exp(-G x): a cash flow seen from a date, given the state.
        template <class T>
        struct Bond
        {
            T amount, G;
        };

        /// Everything the paths read, as functions of the inputs: recorded
        /// before the mark in a differentiable run.
        template <class T>
        struct Setup
        {
            /// Per date.
            std::vector<typename Model<T>::Transition> steps;
            std::vector<T> weight_scale, spot_scale, G_d, G_f;
            /// The flows after each date, as bonds seen from it:
            /// bonds_d[offset_d[i] + j] is flow first_d[i] + j.
            std::vector<Bond<T>> bonds_d, bonds_f;
            std::vector<std::size_t> offset_d, offset_f;

            /// Per option and date before its expiry: both bonds to the
            /// expiry and the deviation the forward still has to accumulate.
            struct OptionDate
            {
                Bond<T> domestic, foreign;
                T deviation;
            };
            std::vector<OptionDate> option_dates;
            std::vector<std::size_t> option_offset;

            /// Per output, the weight of each date.
            std::array<std::vector<T>, kXvaOutputs> weights;
        };

        template <class T>
        void build(const Fixed &f, const Params<T> &p, Setup<T> &s)
        {
            using std::exp;
            using std::sqrt;
            Model<T> m(f, p);
            const std::size_t n = f.times.size();
            const Time horizon = f.times.back();

            // A bond P(t, T) given the state: P(0, T) / P(0, t) e^{-½ G² y(t)} e^{-G x}.
            const auto bond = [&](Curve<T> &curve, const T &G, const T &y, Time t, Time maturity, Real amount)
            {
                return Bond<T>{
                    T(amount * exp(curve.log_discount(maturity) - curve.log_discount(t) - 0.5 * G * G * y)), G};
            };

            Time previous = 0.0;
            for (std::size_t i = 0; i < n; ++i)
            {
                const Time t = f.times[i];
                s.steps.push_back(m.transition(previous, t, horizon));
                const T y_d = m.y_d(t), y_f = m.y_f(t);
                const T G_d = m.G_d(t, horizon), G_f = m.G_f(t, horizon);
                const T convexity_d = 0.5 * G_d * G_d * y_d, convexity_f = 0.5 * G_f * G_f * y_f;
                const T log_d = m.domestic.log_discount(t), log_f = m.foreign.log_discount(t);
                // The discount weight P_d(0, T*) / P_d(t, T*), up to P_d(0, T*),
                // and the spot F(0, T*) e^z P_d(t, T*) / P_f(t, T*).
                s.weight_scale.push_back(T(exp(log_d + convexity_d)));
                s.spot_scale.push_back(T(p.spot * exp(log_f - log_d + convexity_f - convexity_d)));
                s.G_d.push_back(G_d);
                s.G_f.push_back(G_f);

                s.offset_d.push_back(s.bonds_d.size());
                for (std::size_t j = f.first_d[i]; j < f.flows_d.size(); ++j)
                    s.bonds_d.push_back(bond(m.domestic, m.G_d(t, f.flows_d[j].time), y_d, t, f.flows_d[j].time,
                                             f.flows_d[j].amount));
                s.offset_f.push_back(s.bonds_f.size());
                for (std::size_t j = f.first_f[i]; j < f.flows_f.size(); ++j)
                    s.bonds_f.push_back(bond(m.foreign, m.G_f(t, f.flows_f[j].time), y_f, t, f.flows_f[j].time,
                                             f.flows_f[j].amount));
                previous = t;
            }

            for (const Fixed::Option &o : f.options)
            {
                s.option_offset.push_back(s.option_dates.size());
                for (std::size_t i = 0; i < o.expiry_index; ++i)
                {
                    const Time t = f.times[i];
                    s.option_dates.push_back(
                        {bond(m.domestic, m.G_d(t, o.expiry), m.y_d(t), t, o.expiry, 1.0),
                         bond(m.foreign, m.G_f(t, o.expiry), m.y_f(t), t, o.expiry, 1.0),
                         T(sqrt(m.forward_variance(t, o.expiry)))});
                }
            }

            // Default and funding weights of each date (risk/xva.hpp: first
            // to default, and the running funding cost).
            T sc_before(1.0), si_before(1.0);
            previous = 0.0;
            for (std::size_t i = 0; i < n; ++i)
            {
                const Time t = f.times[i];
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

        template <class T>
        struct Workspace
        {
            std::vector<T> x_d, x_f, spot, value;
            explicit Workspace(std::size_t n)
                : x_d(n), x_f(n), spot(n), value(n) {}
        };

        /// Undiscounted Black price on a forward, from the deviation of its
        /// logarithm at the expiry.
        template <class T>
        T black(const T &forward, Real strike, const T &deviation, bool call)
        {
            using std::log;
            if (!(value_of(deviation) > 1e-12))
            {
                const bool in = call ? value_of(forward) > strike : value_of(forward) < strike;
                if (!in)
                    return T(0.0);
                return call ? T(forward - strike) : T(strike - forward);
            }
            const T d1 = log(forward / strike) / deviation + 0.5 * deviation;
            const T d2 = d1 - deviation;
            const T price = call ? T(forward * cdf_of(d1) - strike * cdf_of(d2))
                                 : T(strike * cdf_of(T(-d2)) - forward * cdf_of(T(-d1)));
            // Far out of the money the difference of two rounded products can
            // fall a hair below zero: a price does not.
            return value_of(price) < 0.0 ? T(0.0) : price;
        }

        /// One path: the three factors, the value of the netting set at
        /// every date, and its own CVA, DVA, FCA, FBA and unilateral CVA.
        template <class T>
        void evaluate_path(const Fixed &f, const Setup<T> &s, std::uint64_t path, Workspace<T> &ws,
                           std::array<T, kXvaOutputs> &out)
        {
            using std::exp;
            const std::size_t n = f.times.size();

            // The draws of the pricing engine, stream by stream.
            PhiloxGaussianSource draw_d(f.seed), draw_f(f.seed ^ kForeignStream), draw_s(f.seed ^ kFxStream);
            draw_d.set_path(path);
            draw_f.set_path(path);
            draw_s.set_path(path);
            T d(0.0), x(0.0), z(0.0);
            for (std::size_t i = 0; i < n; ++i)
            {
                const typename Model<T>::Transition &step = s.steps[i];
                const Real z0 = draw_d.next(), z1 = draw_f.next(), z2 = draw_s.next();
                d = step.decay[0] * d + step.drift[0] + step.cholesky[0] * z0;
                x = step.decay[1] * x + step.drift[1] + step.cholesky[1] * z0 + step.cholesky[2] * z1;
                z = z + step.drift[2] + step.cholesky[3] * z0 + step.cholesky[4] * z1 + step.cholesky[5] * z2;
                ws.x_d[i] = d;
                ws.x_f[i] = x;
                ws.spot[i] = s.spot_scale[i] * exp(z - s.G_d[i] * d + s.G_f[i] * x);
            }

            // The known flows: Σ c_d P_d(t, T) + S(t) Σ c_f P_f(t, T).
            for (std::size_t i = 0; i < n; ++i)
            {
                T domestic(0.0), foreign(0.0);
                const std::size_t count_d = f.flows_d.size() - f.first_d[i];
                const Bond<T> *b = s.bonds_d.data() + s.offset_d[i];
                for (std::size_t j = 0; j < count_d; ++j)
                    domestic = domestic + b[j].amount * exp(-b[j].G * ws.x_d[i]);
                const std::size_t count_f = f.flows_f.size() - f.first_f[i];
                b = s.bonds_f.data() + s.offset_f[i];
                for (std::size_t j = 0; j < count_f; ++j)
                    foreign = foreign + b[j].amount * exp(-b[j].G * ws.x_f[i]);
                ws.value[i] = count_f > 0 ? T(domestic + ws.spot[i] * foreign) : domestic;
            }

            // The options: Black on the forward of the path, until the expiry.
            for (std::size_t k = 0; k < f.options.size(); ++k)
            {
                const Fixed::Option &o = f.options[k];
                const typename Setup<T>::OptionDate *dates = s.option_dates.data() + s.option_offset[k];
                for (std::size_t i = 0; i < o.expiry_index; ++i)
                {
                    const T domestic = dates[i].domestic.amount * exp(-dates[i].domestic.G * ws.x_d[i]);
                    const T foreign = dates[i].foreign.amount * exp(-dates[i].foreign.G * ws.x_f[i]);
                    const T forward = ws.spot[i] * foreign / domestic;
                    ws.value[i] = ws.value[i] + o.notional * domestic * black(forward, o.strike, dates[i].deviation, o.call);
                }
            }

            for (T &o : out)
                o = T(0.0);
            for (std::size_t i = 0; i < n; ++i)
            {
                const T v = s.weight_scale[i] * exp(s.G_d[i] * ws.x_d[i]) * ws.value[i];
                if (v > 0.0)
                {
                    out[0] += s.weights[0][i] * v;
                    out[2] += s.weights[2][i] * v;
                    out[4] += s.weights[4][i] * v;
                }
                else if (v < 0.0)
                {
                    out[1] += s.weights[1][i] * v;
                    out[3] += s.weights[3][i] * v;
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

        /// The flows of one currency, netted by payment date and sorted.
        std::vector<Fixed::Flow> netted(const std::map<Time, Real> &by_time)
        {
            std::vector<Fixed::Flow> out;
            for (const auto &[time, amount] : by_time)
                out.push_back({time, amount});
            return out;
        }

        /// For each grid date, the first flow strictly after it.
        std::vector<std::size_t> first_after(const std::vector<Time> &times, const std::vector<Fixed::Flow> &flows)
        {
            std::vector<std::size_t> out;
            std::size_t j = 0;
            for (const Time t : times)
            {
                while (j < flows.size() && flows[j].time <= t + kTimeEps)
                    ++j;
                out.push_back(j);
            }
            return out;
        }

        std::size_t grid_index(const std::vector<Time> &grid, Time t)
        {
            const auto at = std::lower_bound(grid.begin(), grid.end(), t - kTimeEps);
            if (at == grid.end() || std::abs(*at - t) > kTimeEps)
                throw InvalidInput("two-currency xVA risks: an expiry is not on the grid");
            return static_cast<std::size_t>(at - grid.begin());
        }
    } // namespace

    struct CrossCurrencyExposureEngine::RiskRun
    {
        Fixed fixed;
        Params<Real> params;
        std::vector<XvaRiskFactor> factors;
        std::vector<Time> tenors;
        std::vector<std::string> labels;
    };

    void CrossCurrencyExposureEngine::prepare_risk_run(const ExposureSimulationSettings &settings,
                                                       const XvaRiskInputs &inputs, RiskRun &run) const
    {
        if (trades_.empty())
            throw InvalidInput("two-currency engine: no trade");
        if (settings.paths == 0)
            throw InvalidInput("two-currency engine: the number of paths must be > 0");
        if (settings.historical)
            throw InvalidInput("xVA risks: adjustments are prices and need the pricing measure");
        if (inputs.csa)
            throw InvalidInput("two-currency xVA risks: a collateral agreement is not differentiated "
                               "yet; the sensitivities are those of an uncollateralised netting set");
        if (!(inputs.lgd_counterparty >= 0.0 && inputs.lgd_counterparty <= 1.0) ||
            !(inputs.lgd_own >= 0.0 && inputs.lgd_own <= 1.0))
            throw InvalidInput("xVA risks: loss given default must be in [0, 1]");
        if (!std::isfinite(inputs.borrowing_spread) || !std::isfinite(inputs.lending_spread))
            throw InvalidInput("xVA risks: the funding spreads must be finite");

        Fixed &f = run.fixed;
        f.times = grid(settings.grid);
        f.seed = settings.seed;

        // The netting set in flat terms, quantities included.
        std::map<Time, Real> by_time_d, by_time_f;
        for (std::size_t k = 0; k < trades_.size(); ++k)
        {
            xva::TwoFactorTerms terms;
            if (!trades_[k].value->describe(terms))
                throw InvalidInput("two-currency xVA risks: trade " + std::to_string(k) +
                                   " is a rate trade of one currency (a swap, a swaption); the "
                                   "sensitivities cover known cash flows and options on the exchange "
                                   "rate so far");
            const Real q = trades_[k].quantity;
            for (const auto &flow : terms.domestic)
                by_time_d[flow.time] += q * flow.amount;
            for (const auto &flow : terms.foreign)
                by_time_f[flow.time] += q * flow.amount;
            for (const auto &o : terms.options)
                f.options.push_back({o.strike, o.expiry, o.call, q * o.notional, grid_index(f.times, o.expiry)});
        }
        f.flows_d = netted(by_time_d);
        f.flows_f = netted(by_time_f);
        f.first_d = first_after(f.times, f.flows_d);
        f.first_f = first_after(f.times, f.flows_f);

        // The inputs, in the order they are reported in.
        Params<Real> &p = run.params;
        const auto add = [&run](XvaRiskFactor factor, Time tenor, std::string label)
        {
            run.factors.push_back(factor);
            run.tenors.push_back(tenor);
            run.labels.push_back(std::move(label));
        };
        const auto zero_rates = [&add](const DiscountCurve &curve, CurveShape &shape, std::vector<Real> &zero,
                                       XvaRiskFactor factor, const std::string &who)
        {
            shape.pillar_times = curve.pillar_times();
            shape.flat_forward = curve.extrapolation() == CurveExtrapolation::FlatForward;
            if (shape.pillar_times.empty())
            {
                zero.push_back(-std::log(curve.discount(1.0)));
                add(factor, 0.0, who + " zero rate, flat curve");
                return;
            }
            for (std::size_t k = 0; k < shape.pillar_times.size(); ++k)
            {
                zero.push_back(-std::log(curve.pillar_discount_factors()[k]) / shape.pillar_times[k]);
                add(factor, shape.pillar_times[k], who + " zero rate " + years(shape.pillar_times[k]));
            }
        };
        const HullWhiteCurveModel &domestic = model_.domestic(), &foreign = model_.foreign();
        zero_rates(domestic.discount(), f.domestic, p.zero_d, XvaRiskFactor::ZeroRate, "domestic");
        zero_rates(foreign.discount(), f.foreign, p.zero_f, XvaRiskFactor::ForeignZeroRate, "foreign");
        p.a_d = domestic.mean_reversion();
        p.sigma_d = domestic.sigma();
        p.a_f = foreign.mean_reversion();
        p.sigma_f = foreign.sigma();
        p.spot = model_.spot();
        p.sigma_s = model_.fx_volatility();
        p.rho_df = model_.correlations().domestic_foreign;
        p.rho_ds = model_.correlations().domestic_fx;
        p.rho_fs = model_.correlations().foreign_fx;
        add(XvaRiskFactor::MeanReversion, 0.0, "domestic Hull-White mean reversion");
        add(XvaRiskFactor::Sigma, 0.0, "domestic Hull-White volatility");
        add(XvaRiskFactor::ForeignMeanReversion, 0.0, "foreign Hull-White mean reversion");
        add(XvaRiskFactor::ForeignSigma, 0.0, "foreign Hull-White volatility");
        add(XvaRiskFactor::Spot, 0.0, "spot");
        add(XvaRiskFactor::SpotVolatility, 0.0, "spot volatility");
        add(XvaRiskFactor::CorrelationDomesticForeign, 0.0, "correlation of the two rates");
        add(XvaRiskFactor::CorrelationDomesticSpot, 0.0, "correlation of the domestic rate and the spot");
        add(XvaRiskFactor::CorrelationForeignSpot, 0.0, "correlation of the foreign rate and the spot");
        const auto hazards = [&add](const CreditCurve &c, std::vector<Time> &times, std::vector<Real> &values,
                                    XvaRiskFactor factor, const char *who)
        {
            times = c.times();
            values = c.hazards();
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
    }

    XvaValues CrossCurrencyExposureEngine::xva_values(ExposureSimulationSettings settings,
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
        for (std::size_t o = 0; o < kXvaOutputs; ++o)
        {
            std::vector<Real> column(batches);
            for (std::size_t b = 0; b < batches; ++b)
                column[b] = means[b][o];
            result.values[o] = over_batches(column, N);
        }
        return result;
    }

    XvaRisks CrossCurrencyExposureEngine::xva_risks(ExposureSimulationSettings settings,
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
                    // The inputs are the leaves, in the order of Params::all();
                    // the set-up is recorded once, before the mark.
                    const Params<Real> &p = run.params;
                    Params<Number> &q = st.params;
                    for (const Real z : p.zero_d)
                        q.zero_d.emplace_back(z);
                    for (const Real z : p.zero_f)
                        q.zero_f.emplace_back(z);
                    q.a_d = Number(p.a_d);
                    q.sigma_d = Number(p.sigma_d);
                    q.a_f = Number(p.a_f);
                    q.sigma_f = Number(p.sigma_f);
                    q.spot = Number(p.spot);
                    q.sigma_s = Number(p.sigma_s);
                    q.rho_df = Number(p.rho_df);
                    q.rho_ds = Number(p.rho_ds);
                    q.rho_fs = Number(p.rho_fs);
                    for (const Real h : p.hazard_c)
                        q.hazard_c.emplace_back(h);
                    for (const Real h : p.hazard_i)
                        q.hazard_i.emplace_back(h);
                    q.lgd_c = Number(p.lgd_c);
                    q.lgd_i = Number(p.lgd_i);
                    q.spread_b = Number(p.spread_b);
                    q.spread_l = Number(p.spread_l);
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
