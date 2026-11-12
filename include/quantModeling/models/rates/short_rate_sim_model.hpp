#ifndef QM_MODELS_RATES_SHORT_RATE_SIM_MODEL_HPP
#define QM_MODELS_RATES_SHORT_RATE_SIM_MODEL_HPP

#include "quantModeling/core/timegrid.hpp"
#include "quantModeling/core/types.hpp"
#include "quantModeling/models/rates/short_rate_model.hpp"
#include "quantModeling/models/simulation_model.hpp"

#include <cmath>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace quantModeling
{

    /**
     * @brief Any one-factor short-rate model (Vasicek, CIR, Hull-White) as a
     *        script model (WP 26 P3): the catalogue's caplets, caps, floors
     *        and bond options become scripts on it.
     *
     * The hand-written short-rate engine's dynamics, exactly: the model's
     * own Euler step (IShortRateModel::euler_step) on a grid of steps no
     * longer than `max_dt` (1/252 year by default) that holds every event
     * date; the numeraire exp(∫ r dt), the integral by trapezes; and at each
     * event the discount factors the script's df() asks for from the model's
     * closed-form zero-coupon bond, P(t, T, r_t). spot() reads the short
     * rate.
     *
     * Prices only (T = Real): the models have no adjoint form, and the
     * engine gave no greeks.
     */
    class ShortRateSimModel final : public ISimulationModel<Real>
    {
      public:
        explicit ShortRateSimModel(std::shared_ptr<const IShortRateModel> model, Time max_dt = 1.0 / 252.0)
            : model_(std::move(model)), max_dt_(max_dt)
        {
            if (!model_)
                throw InvalidInput("ShortRateSimModel: no short-rate model");
            if (!(max_dt_ > 0.0))
                throw InvalidInput("ShortRateSimModel: max_dt must be positive");
        }

        std::size_t n_underlyings() const override { return 1; }

        void init(const TimeLine &product_timeline, const std::vector<SampleDef> &defline) override
        {
            refuse_barrier_monitoring(defline, "ShortRateSimModel");
            for (const SampleDef &def : defline)
                if (!def.forward_mats.empty())
                    throw InvalidInput("ShortRateSimModel: no forward lookups (df() only)");
            timeline_ = canonical_timeline(product_timeline);
            sim_timeline_ = add_monitoring_steps(timeline_, max_dt_);
            defline_ = defline;
            event_index_of_step_ = event_index_of_step(sim_timeline_, timeline_);
        }

        const TimeLine &sim_timeline() const override { return sim_timeline_; }
        std::size_t sim_dim() const override { return sim_timeline_.size(); }
        BrownianLayout brownian_layout() const override { return BrownianLayout{sim_timeline_, 1, 1}; }

        void generate_path(std::span<const double> gaussians, Scenario<Real> &path) const override
        {
            Real r = model_->r0();
            Real integral = 0.0;
            Time t = 0.0;
            for (std::size_t k = 0; k < sim_timeline_.size(); ++k)
            {
                const Time dt = sim_timeline_[k] - t;
                const Real r_old = r;
                r = model_->euler_step(r, t, dt, std::sqrt(dt) * gaussians[k]);
                t = sim_timeline_[k];
                integral += 0.5 * (r_old + r) * dt;

                const std::ptrdiff_t event = event_index_of_step_[k];
                if (event < 0)
                    continue;
                Sample<Real> &smp = path[static_cast<std::size_t>(event)];
                const SampleDef &def = defline_[static_cast<std::size_t>(event)];
                smp.spots.assign(1, r);
                smp.numeraire = std::exp(integral);
                smp.discounts.resize(def.discount_mats.size());
                for (std::size_t m = 0; m < def.discount_mats.size(); ++m)
                    smp.discounts[m] = model_->zcb_price(t, def.discount_mats[m], r);
            }
        }

        std::unique_ptr<ISimulationModel<Real>> clone() const override
        {
            return std::make_unique<ShortRateSimModel>(*this);
        }

        const std::vector<Real *> &parameters() const override { return no_params_; }
        const std::vector<std::string> &parameter_labels() const override { return no_labels_; }

      private:
        std::shared_ptr<const IShortRateModel> model_; ///< immutable: clones share it
        Time max_dt_;
        TimeLine timeline_, sim_timeline_;
        std::vector<SampleDef> defline_;
        std::vector<std::ptrdiff_t> event_index_of_step_;
        std::vector<Real *> no_params_;
        std::vector<std::string> no_labels_;
    };

} // namespace quantModeling

#endif // QM_MODELS_RATES_SHORT_RATE_SIM_MODEL_HPP
