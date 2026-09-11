#ifndef QM_MODELS_EQUITY_BS_SIM_MODEL_HPP
#define QM_MODELS_EQUITY_BS_SIM_MODEL_HPP

#include "quantModeling/core/types.hpp"
#include "quantModeling/market/discount_curve.hpp"
#include "quantModeling/models/equity/black_scholes.hpp"
#include "quantModeling/models/simulation_model.hpp"

#include <cmath>
#include <cstddef>
#include <memory>
#include <span>
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
     * flat number — the piece a forward-starting product (a cliquet resetting
     * its strike at a future date, an autocall observed for years) actually
     * needs discounted correctly. The curve is a non-owning reference: the
     * caller keeps it alive for the model's lifetime (ValuationContext's
     * DayCounter/Calendar pointers are the same convention).
     */
    template <class T = Real>
    class BlackScholesSimModel final : public ISimulationModel<T>
    {
      public:
        BlackScholesSimModel(Real s0, Real r, Real q, Real sigma)
            : s0_(s0), r_(r), q_(q), sigma_(sigma)
        {
        }

        BlackScholesSimModel(Real s0, const DiscountCurve &curve, Real q,
                             Real sigma)
            : s0_(s0), q_(q), sigma_(sigma), curve_(&curve)
        {
        }

        explicit BlackScholesSimModel(const ILocalVolModel &m)
            : s0_(m.spot0()), r_(m.rate_r()), q_(m.yield_q()),
              sigma_(m.vol_sigma())
        {
        }

        std::size_t n_underlyings() const override { return 1; }

        void init(const TimeLine &product_timeline,
                  const std::vector<SampleDef> &defline) override
        {
            timeline_ = canonical_timeline(product_timeline);
            defline_ = defline;

            const Real mu = r_ - q_ - 0.5 * sigma_ * sigma_; // flat-rate case only
            steps_.clear();
            sim_dim_ = 0;
            Real t_prev = 0.0;
            Real log_df_prev = 0.0; // ln DF(0) = 0
            for (const Time t : timeline_)
            {
                Step s;
                s.t = t;
                s.draws = (t > TIMELINE_EPS);
                if (s.draws)
                {
                    const Real dt = t - t_prev;
                    if (curve_)
                    {
                        const Real log_df_t = std::log(curve_->discount(t));
                        s.drift = (log_df_prev - log_df_t) - q_ * dt -
                                 0.5 * sigma_ * sigma_ * dt;
                        log_df_prev = log_df_t;
                    }
                    else
                    {
                        s.drift = mu * dt;
                    }
                    s.vol_sqrt_dt = sigma_ * std::sqrt(dt);
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
            Real S = s0_;
            std::size_t g = 0;
            for (std::size_t i = 0; i < steps_.size(); ++i)
            {
                const Step &st = steps_[i];
                if (st.draws)
                {
                    const double z = gaussians[g++];
                    S *= std::exp(st.drift + st.vol_sqrt_dt * z);
                }

                Sample<T> &smp = path[i];
                smp.spots.assign(1, T(S));
                smp.numeraire = curve_ ? T(1.0 / curve_->discount(st.t))
                                       : T(std::exp(r_ * st.t));

                const SampleDef &def = defline_[i];
                smp.discounts.resize(def.discount_mats.size());
                for (std::size_t k = 0; k < def.discount_mats.size(); ++k)
                    smp.discounts[k] = discount_between(st.t, def.discount_mats[k]);

                smp.forwards.resize(def.forward_mats.size());
                for (std::size_t k = 0; k < def.forward_mats.size(); ++k)
                {
                    const Time mat = def.forward_mats[k];
                    const Real growth =
                        curve_ ? curve_->discount(st.t) / curve_->discount(mat)
                               : std::exp(r_ * (mat - st.t));
                    smp.forwards[k] = T(S * growth * std::exp(-q_ * (mat - st.t)));
                }
            }
        }

        std::unique_ptr<ISimulationModel<T>> clone() const override
        {
            return std::make_unique<BlackScholesSimModel<T>>(*this);
        }

      private:
        struct Step
        {
            Time t = 0.0;
            bool draws = false;
            Real drift = 0.0;
            Real vol_sqrt_dt = 0.0;
        };

        /// P(t, T) — from the curve if there is one, else the flat formula.
        T discount_between(Time t, Time maturity) const
        {
            if (curve_)
                return T(curve_->discount(maturity) / curve_->discount(t));
            return T(std::exp(-r_ * (maturity - t)));
        }

        Real s0_, r_ = 0.0, q_, sigma_;
        const DiscountCurve *curve_ = nullptr; // non-owning; nullptr => flat r_
        TimeLine timeline_;
        std::vector<SampleDef> defline_;
        std::vector<Step> steps_;
        std::size_t sim_dim_ = 0;
    };

} // namespace quantModeling

#endif // QM_MODELS_EQUITY_BS_SIM_MODEL_HPP
