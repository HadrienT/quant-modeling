#ifndef QM_MODELS_EQUITY_CURVE_CARRY_HPP
#define QM_MODELS_EQUITY_CURVE_CARRY_HPP

#include "quantModeling/core/timegrid.hpp"
#include "quantModeling/core/types.hpp"
#include "quantModeling/market/forward_curve.hpp"
#include "quantModeling/models/simulation_model.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
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
     * Everything here is double: the curve is market data, not a model
     * parameter, here.
     */
    class CurveCarry
    {
      public:
        explicit CurveCarry(ForwardCurve curve)
            : curve_(std::move(curve)) {}

        const ForwardCurve &curve() const { return curve_; }

        /// The carry rates of the steps (steps[k − 1], steps[k]], from 0 for
        /// k = 0; zero on a step of no length.
        void set_steps(const TimeLine &steps)
        {
            step_rate_.resize(steps.size());
            Time prev = 0.0;
            for (std::size_t k = 0; k < steps.size(); ++k)
            {
                const Time dt = steps[k] - prev;
                step_rate_[k] = dt > 0.0 ? curve_.carry(prev, steps[k]) / dt : 0.0;
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
            for (std::size_t e = 0; e < n; ++e)
            {
                const Time t = events[e];
                const Real df_t = curve_.discount(t);
                inv_df_[e] = 1.0 / df_t;
                discounts_[e].clear();
                for (const Time m : defline[e].discount_mats)
                    discounts_[e].push_back(curve_.discount(m) / df_t);
                growths_[e].clear();
                for (const Time m : defline[e].forward_mats)
                    growths_[e].push_back(std::exp(curve_.carry(t, m)));
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

      private:
        ForwardCurve curve_;
        std::vector<Real> step_rate_;
        std::vector<Real> inv_df_;
        std::vector<std::vector<Real>> discounts_;
        std::vector<std::vector<Real>> growths_;
    };

    /// Event e's numeraire, discounts and forwards on the curve, r and q as
    /// shifts over it, for a spot S at the event's date t.
    template <class T>
    void write_curve_rates(const CurveCarry &c, const SampleDef &def, std::size_t e, Time t, const T &S, const T &r,
                           const T &q, Sample<T> &smp)
    {
        using std::exp;
        smp.numeraire = T(c.inv_discount(e)) * exp(r * t);
        smp.discounts.resize(def.discount_mats.size());
        for (std::size_t m = 0; m < def.discount_mats.size(); ++m)
            smp.discounts[m] = T(c.discount(e, m)) * exp(-r * (def.discount_mats[m] - t));
        smp.forwards.resize(def.forward_mats.size());
        for (std::size_t m = 0; m < def.forward_mats.size(); ++m)
            smp.forwards[m] = S * T(c.growth(e, m)) * exp((r - q) * (def.forward_mats[m] - t));
    }

} // namespace quantModeling

#endif // QM_MODELS_EQUITY_CURVE_CARRY_HPP
