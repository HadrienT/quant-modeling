#ifndef QM_MODELS_EQUITY_BS_SIM_MODEL_HPP
#define QM_MODELS_EQUITY_BS_SIM_MODEL_HPP

#include "quantModeling/aad/number.hpp"
#include "quantModeling/core/types.hpp"
#include "quantModeling/market/discount_curve.hpp"
#include "quantModeling/models/equity/black_scholes.hpp"
#include "quantModeling/models/simulation_model.hpp"

#include <cmath>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace quantModeling
{

    /**
     * @brief Single-asset Black-Scholes for the timeline simulation
     *        architecture — a flat rate, or a bootstrapped term structure.
     *
     * GBM has an exact transition density, so the simulation timeline is just
     * the product's event dates — no intermediate Euler steps. One standard
     * normal is drawn per event date with t > 0.
     *
     * With a flat rate r:
     *   S(t_i) = S(t_{i-1}) exp((r - q - 0.5 sigma^2) dt + sigma sqrt(dt) Z_i)
     *   numeraire(t_i)   = exp(r t_i)          (deflator = 1/numeraire)
     *   discount(t_i, T) = exp(-r (T - t_i))
     *   forward(t_i, T)  = S(t_i) exp((r - q)(T - t_i))
     *
     * With a DiscountCurve, r is replaced everywhere by the curve's own
     * forward rate: the drift over one step is the curve's forward rate
     * integrated over that step, `ln DF(t_{i-1}) - ln DF(t_i)`, so a path
     * grows consistently with *today's* term structure instead of a single
     * flat number. The curve itself is always double, non-owning, and not a
     * differentiable parameter here (its own risk -- the "superbucket" --
     * is blueprint/wp/17-aad.md §11, later and harder).
     *
     * s0_, r_, q_, sigma_ are T: with T = aad::Number, every one of them is a
     * differentiable model parameter (blueprint §6.2). Everything that does
     * NOT depend on a parameter -- the timeline, dt, the gaussian draws --
     * stays double (blueprint's table in §6.1). Every call to exp/log/sqrt on
     * a T is unqualified (`using std::exp;` below) so that, for T = Number,
     * argument-dependent lookup finds aad::exp instead of the double-only
     * std::exp -- see blueprint §5.5: std::exp(Number) does not compile
     * (Number's conversion to double is explicit), which is the *safe*
     * failure mode of forgetting this.
     */
    template <class T = Real>
    class BlackScholesSimModel final : public ISimulationModel<T>
    {
      public:
        BlackScholesSimModel(T s0, T r, T q, T sigma)
            : s0_(s0), r_(r), q_(q), sigma_(sigma)
        {
            set_param_pointers();
        }

        BlackScholesSimModel(T s0, const DiscountCurve &curve, T q, T sigma)
            : s0_(s0), q_(q), sigma_(sigma), curve_(&curve)
        {
            set_param_pointers();
        }

        explicit BlackScholesSimModel(const ILocalVolModel &m)
            : s0_(m.spot0()), r_(m.rate_r()), q_(m.yield_q()),
              sigma_(m.vol_sigma())
        {
            set_param_pointers();
        }

        /// The book's trap, closed: params_ caches pointers into *this*
        /// object's own s0_/r_/q_/sigma_, so a naive compiler-generated copy
        /// would leave the copy's params_ pointing at the ORIGINAL's members
        /// -- clone() would then hand back a model whose sensitivities are
        /// silently computed against the wrong object. Recomputing the
        /// pointers in a hand-written copy constructor is the fix.
        BlackScholesSimModel(const BlackScholesSimModel &other)
            : s0_(other.s0_), r_(other.r_), q_(other.q_), sigma_(other.sigma_),
              curve_(other.curve_), timeline_(other.timeline_),
              defline_(other.defline_), steps_(other.steps_),
              sim_dim_(other.sim_dim_)
        {
            set_param_pointers();
        }

        std::size_t n_underlyings() const override { return 1; }

        void init(const TimeLine &product_timeline,
                  const std::vector<SampleDef> &defline) override
        {
            using std::exp;
            using std::log;
            using std::sqrt;

            timeline_ = canonical_timeline(product_timeline);
            defline_ = defline;

            const T mu = r_ - q_ - 0.5 * sigma_ * sigma_; // flat-rate case only
            steps_.clear();
            sim_dim_ = 0;
            Time t_prev = 0.0;
            Real log_df_prev = 0.0; // ln DF(0) = 0 -- the curve is always double
            for (const Time t : timeline_)
            {
                Step s;
                s.t = t;
                s.draws = (t > TIMELINE_EPS);
                if (s.draws)
                {
                    const Time dt = t - t_prev;
                    if (curve_)
                    {
                        const Real log_df_t = log(curve_->discount(t));
                        s.drift = (log_df_prev - log_df_t) - q_ * dt -
                                 0.5 * sigma_ * sigma_ * dt;
                        log_df_prev = log_df_t;
                    }
                    else
                    {
                        s.drift = mu * dt;
                    }
                    s.vol_sqrt_dt = sigma_ * sqrt(dt);
                    ++sim_dim_;
                }
                steps_.push_back(s);
                t_prev = t;
            }
        }

        const TimeLine &sim_timeline() const override { return timeline_; }
        std::size_t sim_dim() const override { return sim_dim_; }

        void generate_path(std::span<const double> gaussians,
                           Scenario<T> &path) const override
        {
            using std::exp;

            T S = s0_;
            std::size_t g = 0;
            for (std::size_t i = 0; i < steps_.size(); ++i)
            {
                const Step &st = steps_[i];
                if (st.draws)
                {
                    const double z = gaussians[g++];
                    S *= exp(st.drift + st.vol_sqrt_dt * z);
                }

                Sample<T> &smp = path[i];
                smp.spots.assign(1, S);
                smp.numeraire = curve_ ? T(1.0 / curve_->discount(st.t))
                                       : T(exp(r_ * st.t));

                const SampleDef &def = defline_[i];
                smp.discounts.resize(def.discount_mats.size());
                for (std::size_t k = 0; k < def.discount_mats.size(); ++k)
                    smp.discounts[k] = discount_between(st.t, def.discount_mats[k]);

                smp.forwards.resize(def.forward_mats.size());
                for (std::size_t k = 0; k < def.forward_mats.size(); ++k)
                {
                    const Time mat = def.forward_mats[k];
                    const T growth = curve_ ? T(curve_->discount(st.t) / curve_->discount(mat))
                                            : T(exp(r_ * (mat - st.t)));
                    smp.forwards[k] = S * growth * exp(-q_ * (mat - st.t));
                }
            }
        }

        std::unique_ptr<ISimulationModel<T>> clone() const override
        {
            return std::make_unique<BlackScholesSimModel<T>>(*this);
        }

        const std::vector<T *> &parameters() const override { return params_; }
        const std::vector<std::string> &parameter_labels() const override
        {
            static const std::vector<std::string> labels{"spot", "rate", "div", "vol"};
            return labels;
        }

      private:
        struct Step
        {
            Time t = 0.0;
            bool draws = false;
            T drift{};
            T vol_sqrt_dt{};
        };

        void set_param_pointers() { params_ = {&s0_, &r_, &q_, &sigma_}; }

        /// P(t, T) — from the curve if there is one, else the flat formula.
        T discount_between(Time t, Time maturity) const
        {
            using std::exp;
            if (curve_)
                return T(curve_->discount(maturity) / curve_->discount(t));
            return exp(-r_ * (maturity - t));
        }

        T s0_{}, r_{}, q_{}, sigma_{};
        const DiscountCurve *curve_ = nullptr; // non-owning; nullptr => flat r_
        TimeLine timeline_;
        std::vector<SampleDef> defline_;
        std::vector<Step> steps_;
        std::size_t sim_dim_ = 0;
        std::vector<T *> params_; // set_param_pointers() keeps this current
    };

} // namespace quantModeling

#endif // QM_MODELS_EQUITY_BS_SIM_MODEL_HPP
