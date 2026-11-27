#ifndef QM_MODELS_EQUITY_CURVE_CARRY_HPP
#define QM_MODELS_EQUITY_CURVE_CARRY_HPP

#include "quantModeling/core/timegrid.hpp"
#include "quantModeling/core/types.hpp"
#include "quantModeling/market/forward_curve.hpp"
#include "quantModeling/models/simulation_model.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace quantModeling
{

    /**
     * @brief A forward curve as a simulation model reads it (WP 32 F2):
     *        tables computed once in init(), so that a path does no curve
     *        lookup.
     *
     * A model on a curve keeps its scalar rate r and dividend q as parallel
     * shifts over it (zero to price on the curve itself), so that its rho
     * and dividend risk stay the sums of the per-pillar risks of F3:
     *
     *   step k's rate argument  = c_k + r, c_k = ln F(t_k)/F(t_{k−1}) / Δt_k
     *   numeraire(t_e)          = e^{r t_e} / DF(t_e)
     *   discount(t_e, m)        = DF(m)/DF(t_e) · e^{−r (m − t_e)}
     *   forward(t_e, m) / S     = F(m)/F(t_e)   · e^{(r − q)(m − t_e)}
     *
     * so a step's drift (c_k + r − q − σ²/2) Δt telescopes onto ln F(t)/S0.
     * Everything here is double: the curve is market data.
     *
     * The risks per pillar (WP 32 F3) are the derivatives in shifts of the
     * pillars, each a model parameter at zero (ModelCurve below): the zero
     * rate of discount pillar j by δ_j, the dividend yield of forward pillar
     * i by ε_i,
     *
     *   Δ ln DF(t) = −Σ_j w_j(t) t_j δ_j
     *   Δ ln F(t)  = +Σ_j w_j(t) t_j δ_j − Σ_i u_i(t) t_i ε_i
     *
     * with w and u the curve's own interpolation weights
     * (DiscountCurve::log_discount_weights, ForwardCurve::log_forward_weights).
     * A rate pillar moves the forward with the discount, as the scalar rate
     * does; both interpolations reproduce a linear function of t, so equal
     * shifts of every pillar are the parallel shift and the pillar risks sum
     * to the scalar rho and dividend risk. The weights are local, so each
     * step and each event reads two to four pillars: the tables below hold
     * them sparse.
     */
    class CurveCarry
    {
      public:
        /// One pillar shift's coefficient in a log quantity.
        struct Term
        {
            std::uint32_t index;
            Real coef;
        };
        using Terms = std::vector<Term>;

        explicit CurveCarry(ForwardCurve curve)
            : curve_(std::move(curve))
        {
            const std::size_t nd = curve_.discount_curve().pillar_times().size();
            const std::size_t nf = curve_.pillar_times().size();
            for (std::size_t j = 0; j < nd; ++j)
                labels_.push_back("rate[" + std::to_string(j) + "]");
            for (std::size_t i = 0; i < nf; ++i)
                labels_.push_back("div[" + std::to_string(i) + "]");
        }

        const ForwardCurve &curve() const { return curve_; }

        /// The carry rates of the steps (steps[k − 1], steps[k]], from 0 for
        /// k = 0; zero on a step of no length.
        void set_steps(const TimeLine &steps)
        {
            step_rate_.resize(steps.size());
            step_terms_.resize(steps.size());
            Time prev = 0.0;
            for (std::size_t k = 0; k < steps.size(); ++k)
            {
                const Time dt = steps[k] - prev;
                step_rate_[k] = dt > 0.0 ? curve_.carry(prev, steps[k]) / dt : 0.0;
                step_terms_[k] = dt > 0.0 ? difference(log_forward_terms(steps[k]), log_forward_terms(prev), 1.0 / dt)
                                          : Terms{};
                prev = steps[k];
            }
        }

        /// The steps of a uniform grid, k dt for k = 1..n (rough Bergomi):
        /// step_rate(k − 1) is step k's.
        void set_uniform_steps(std::size_t n, Time dt)
        {
            TimeLine steps(n);
            for (std::size_t k = 0; k < n; ++k)
                steps[k] = static_cast<Real>(k + 1) * dt;
            set_steps(steps);
        }

        Real step_rate(std::size_t k) const { return step_rate_[k]; }

        /// The events' numeraires, discounts and forward growths.
        void set_events(const TimeLine &events, const std::vector<SampleDef> &defline)
        {
            const std::size_t n = std::min(events.size(), defline.size());
            inv_df_.resize(n);
            discounts_.resize(n);
            growths_.resize(n);
            inv_df_terms_.resize(n);
            discount_terms_.resize(n);
            growth_terms_.resize(n);
            for (std::size_t e = 0; e < n; ++e)
            {
                const Time t = events[e];
                const Real df_t = curve_.discount(t);
                inv_df_[e] = 1.0 / df_t;
                inv_df_terms_[e] = difference({}, log_discount_terms(t), 1.0);
                discounts_[e].clear();
                discount_terms_[e].clear();
                for (const Time m : defline[e].discount_mats)
                {
                    discounts_[e].push_back(curve_.discount(m) / df_t);
                    discount_terms_[e].push_back(difference(log_discount_terms(m), log_discount_terms(t), 1.0));
                }
                growths_[e].clear();
                growth_terms_[e].clear();
                for (const Time m : defline[e].forward_mats)
                {
                    growths_[e].push_back(std::exp(curve_.carry(t, m)));
                    growth_terms_[e].push_back(difference(log_forward_terms(m), log_forward_terms(t), 1.0));
                }
            }
        }

        /// 1 / DF(t_e).
        Real inv_discount(std::size_t e) const { return inv_df_[e]; }
        /// DF(m) / DF(t_e) for the event's m-th discount maturity.
        Real discount(std::size_t e, std::size_t m) const { return discounts_[e][m]; }
        /// F(m) / F(t_e) for the event's m-th forward maturity.
        Real growth(std::size_t e, std::size_t m) const { return growths_[e][m]; }

        /// E[S(t)/N(t)] / S0 on the curve: F(t) DF(t) / S0 (times e^{−q t}
        /// for a dividend shift q).
        Real deflated_spot_ratio(Time t) const
        {
            return curve_.forward(t) * curve_.discount(t) / curve_.spot();
        }

        /// The pillar shifts: the discount pillars' rates, then the forward
        /// pillars' dividend yields ("rate[j]", "div[i]").
        std::size_t n_shifts() const { return labels_.size(); }
        const std::vector<std::string> &shift_labels() const { return labels_; }
        /// The shifts' coefficients in step k's rate argument, in ln(1/DF(t_e)),
        /// ln(DF(m)/DF(t_e)) and ln(F(m)/F(t_e)).
        const Terms &step_terms(std::size_t k) const { return step_terms_[k]; }
        const Terms &inv_discount_terms(std::size_t e) const { return inv_df_terms_[e]; }
        const Terms &discount_terms(std::size_t e, std::size_t m) const { return discount_terms_[e][m]; }
        const Terms &growth_terms(std::size_t e, std::size_t m) const { return growth_terms_[e][m]; }

      private:
        /// Δ ln DF(t) in the shifts, dense.
        std::vector<Real> log_discount_terms(Time t) const
        {
            std::vector<Real> d(n_shifts(), 0.0);
            const std::vector<Time> &tj = curve_.discount_curve().pillar_times();
            curve_.discount_curve().log_discount_weights(t, weights_);
            for (const auto &[j, w] : weights_)
                d[j] -= w * tj[j];
            return d;
        }

        /// Δ ln F(t) in the shifts, dense.
        std::vector<Real> log_forward_terms(Time t) const
        {
            std::vector<Real> d = log_discount_terms(t);
            for (Real &x : d)
                x = -x; // a rate pillar moves the forward with the discount
            const std::size_t nd = curve_.discount_curve().pillar_times().size();
            const std::vector<Time> &ti = curve_.pillar_times();
            curve_.log_forward_weights(t, weights_);
            for (const auto &[i, u] : weights_)
                d[nd + i] -= u * ti[i];
            return d;
        }

        /// scale (a − b), sparse; an empty a or b is zero.
        Terms difference(const std::vector<Real> &a, const std::vector<Real> &b, Real scale) const
        {
            Terms out;
            for (std::size_t i = 0; i < n_shifts(); ++i)
            {
                const Real x = scale * ((a.empty() ? 0.0 : a[i]) - (b.empty() ? 0.0 : b[i]));
                if (x != 0.0)
                    out.push_back({static_cast<std::uint32_t>(i), x});
            }
            return out;
        }

        ForwardCurve curve_;
        std::vector<std::string> labels_;
        std::vector<Real> step_rate_;
        std::vector<Real> inv_df_;
        std::vector<std::vector<Real>> discounts_;
        std::vector<std::vector<Real>> growths_;
        std::vector<Terms> step_terms_;
        std::vector<Terms> inv_df_terms_;
        std::vector<std::vector<Terms>> discount_terms_;
        std::vector<std::vector<Terms>> growth_terms_;
        mutable std::vector<std::pair<std::size_t, Real>> weights_; // scratch, init() only
    };

    /**
     * @brief The curve a model of number type T steps on: CurveCarry's
     *        tables and the pillar shifts as T parameters at zero (WP 32 F3).
     *
     * With T = Real the shifts are not read: the paths are F2's, bit for bit.
     * With an AAD number, each step's rate argument and each event's log
     * numeraire, discounts and forwards add their two to four pillars'
     * shifts, so the tape carries the pillar risks.
     */
    template <class T>
    class ModelCurve
    {
      public:
        explicit ModelCurve(const ForwardCurve &curve)
            : tables_(curve)
        {
            // One leaf each: copies of one Number would share its node.
            shifts_.reserve(tables_.n_shifts());
            for (std::size_t i = 0; i < tables_.n_shifts(); ++i)
                shifts_.emplace_back(0.0);
        }

        void set_steps(const TimeLine &steps) { tables_.set_steps(steps); }
        void set_uniform_steps(std::size_t n, Time dt) { tables_.set_uniform_steps(n, dt); }
        void set_events(const TimeLine &events, const std::vector<SampleDef> &defline)
        {
            tables_.set_events(events, defline);
        }
        Real deflated_spot_ratio(Time t) const { return tables_.deflated_spot_ratio(t); }

        std::vector<T> &shifts() { return shifts_; }
        const std::vector<std::string> &shift_labels() const { return tables_.shift_labels(); }

        /// The pillar shifts after a model's own parameters (set_param_pointers).
        void append_parameters(std::vector<T *> &params, std::vector<std::string> &labels)
        {
            for (std::size_t i = 0; i < shifts_.size(); ++i)
            {
                params.push_back(&shifts_[i]);
                labels.push_back(tables_.shift_labels()[i]);
            }
        }

        /// Step k's rate argument: the curve's carry rate over it plus r.
        T step_rate(std::size_t k, const T &r) const
        {
            if constexpr (std::is_same_v<T, Real>)
                return T(tables_.step_rate(k)) + r;
            else
                return with(T(T(tables_.step_rate(k)) + r), tables_.step_terms(k), 1.0);
        }

        /// Step k's log carry over a step of length dt, c_k dt (shifts
        /// included), for a model that adds it to its own drift.
        T step_log_carry(std::size_t k, Time dt) const
        {
            if constexpr (std::is_same_v<T, Real>)
                return tables_.step_rate(k) * dt;
            else
                return with(T(tables_.step_rate(k) * dt), tables_.step_terms(k), dt);
        }

        /// Event e's numeraire, discounts and forwards on the curve, r and q
        /// as parallel shifts over it, for a spot S at the event's date t.
        void write_rates(const SampleDef &def, std::size_t e, Time t, const T &S, const T &r, const T &q,
                         Sample<T> &smp) const
        {
            using std::exp;
            const CurveCarry &c = tables_;
            smp.numeraire = T(c.inv_discount(e)) * exp(with(T(r * t), c.inv_discount_terms(e), 1.0));
            smp.discounts.resize(def.discount_mats.size());
            for (std::size_t m = 0; m < def.discount_mats.size(); ++m)
                smp.discounts[m] =
                    T(c.discount(e, m)) * exp(with(T(-r * (def.discount_mats[m] - t)), c.discount_terms(e, m), 1.0));
            smp.forwards.resize(def.forward_mats.size());
            for (std::size_t m = 0; m < def.forward_mats.size(); ++m)
                smp.forwards[m] =
                    S * T(c.growth(e, m)) *
                    exp(with(T((r - q) * (def.forward_mats[m] - t)), c.growth_terms(e, m), 1.0));
        }

      private:
        /// x + scale Σ coef · shift, on the tape only.
        T with(T x, const CurveCarry::Terms &terms, Real scale) const
        {
            if constexpr (!std::is_same_v<T, Real>)
                for (const CurveCarry::Term &term : terms)
                    x = x + (scale * term.coef) * shifts_[term.index];
            return x;
        }

        CurveCarry tables_;
        std::vector<T> shifts_;
    };

} // namespace quantModeling

#endif // QM_MODELS_EQUITY_CURVE_CARRY_HPP
