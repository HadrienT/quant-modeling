#ifndef QM_MODELS_EQUITY_BS_SIM_MODEL_HPP
#define QM_MODELS_EQUITY_BS_SIM_MODEL_HPP

#include "quantModeling/core/types.hpp"
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
     * @brief Single-asset Black-Scholes (flat r, q, sigma) for the timeline
     *        simulation architecture.
     *
     * GBM has an exact transition density, so the simulation timeline is just
     * the product's event dates — no intermediate Euler steps. One standard
     * normal is drawn per event date with t > 0.
     *
     *   S(t_i) = S(t_{i-1}) exp((r - q - 0.5 sigma^2) dt + sigma sqrt(dt) Z_i)
     *
     * numeraire(t_i)   = exp(r t_i)                      (deflator = 1/numeraire)
     * discount(t_i, T) = exp(-r (T - t_i))
     * forward(t_i, T)  = S(t_i) exp((r - q)(T - t_i))
     */
    template <class T = Real>
    class BlackScholesSimModel final : public ISimulationModel<T>
    {
      public:
        BlackScholesSimModel(Real s0, Real r, Real q, Real sigma)
            : s0_(s0), r_(r), q_(q), sigma_(sigma)
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

            const Real mu = r_ - q_ - 0.5 * sigma_ * sigma_;
            steps_.clear();
            sim_dim_ = 0;
            Real t_prev = 0.0;
            for (const Time t : timeline_)
            {
                Step s;
                s.t = t;
                s.draws = (t > TIMELINE_EPS);
                if (s.draws)
                {
                    const Real dt = t - t_prev;
                    s.drift = mu * dt;
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
                smp.numeraire = T(std::exp(r_ * st.t));

                const SampleDef &def = defline_[i];
                smp.discounts.resize(def.discount_mats.size());
                for (std::size_t k = 0; k < def.discount_mats.size(); ++k)
                    smp.discounts[k] =
                        T(std::exp(-r_ * (def.discount_mats[k] - st.t)));

                smp.forwards.resize(def.forward_mats.size());
                for (std::size_t k = 0; k < def.forward_mats.size(); ++k)
                    smp.forwards[k] =
                        T(S * std::exp((r_ - q_) * (def.forward_mats[k] - st.t)));
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

        Real s0_, r_, q_, sigma_;
        TimeLine timeline_;
        std::vector<SampleDef> defline_;
        std::vector<Step> steps_;
        std::size_t sim_dim_ = 0;
    };

} // namespace quantModeling

#endif // QM_MODELS_EQUITY_BS_SIM_MODEL_HPP
