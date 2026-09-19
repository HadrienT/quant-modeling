#ifndef QM_MODELS_EQUITY_SABR_SIM_MODEL_HPP
#define QM_MODELS_EQUITY_SABR_SIM_MODEL_HPP

#include "quantModeling/aad/number.hpp"
#include "quantModeling/core/timegrid.hpp"
#include "quantModeling/core/types.hpp"
#include "quantModeling/models/simulation_model.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace quantModeling
{

    /**
     * @brief SABR (Hagan, Kumar, Lesniewski, Woodward, 2002) for the timeline
     *        simulation architecture.
     *
     *   dF     = alpha * F^beta dW1
     *   dalpha = nu * alpha dW2,   dW1 dW2 = rho dt
     *
     * F is a forward price, not a spot: it is already a Q-martingale by
     * construction (no drift term at all -- unlike every other model in
     * this file, there is no r/q on F's own SDE). r only enters here to
     * build the numeraire/discount convention this architecture's NodePays
     * and Sample::discounts expect, exactly the exp(r*t) every other model
     * uses -- in a flat, deterministic-rate world (the only kind this
     * codebase has today) a forward is a Q-martingale for the same reason
     * it is a martingale under its own T-forward measure, so this needs no
     * separate treatment: `spots[0]` plays exactly the role "spot()" plays
     * everywhere else, and a European payoff scripted the ordinary way
     * prices to the same number Black-76 + Hagan's own implied vol would
     * give (see the test file's cross-check against sabr_implied_vol() /
     * black76_call_price(), the pre-existing, independently-tested oracle).
     *
     * beta is deliberately NOT T: models/equity/sabr.hpp's own doc comment
     * notes it is conventionally fixed exogenously rather than calibrated
     * (a single smile snapshot cannot jointly identify alpha and beta), so
     * it is treated the same way LocalVolSimModel treats its K/T grid axes
     * -- a structural constant, not a risk factor.
     *
     * alpha's own SDE, dalpha/alpha = nu dW2, is a driftless lognormal
     * process with a *constant* (not stochastic) vol nu: exactly Black-
     * Scholes's own SDE shape, so it admits the same one-big-step-per-event
     * exact update Black-Scholes uses -- no discretisation error on alpha
     * at all, regardless of step size. F's own SDE has no such luck: alpha
     * multiplying it is itself stochastic, and F^beta is nonlinear for
     * beta != 1, so F needs a genuine Euler scheme on a fine internal grid
     * (add_monitoring_steps(), the same tool LocalVolSimModel and
     * BatesSimModel use). Absorption at F = 0 (standard for a CEV-type
     * process with beta < 1: F^beta of a negative F is not defined) is
     * handled the same way Bates floors variance -- clamp before raising to
     * the power, propagate the (possibly zero) result forward -- except
     * here F = 0 is a true absorbing barrier of the real process, not a
     * numerical patch: once hit, dF = alpha * 0^beta * dW1 = 0 forever
     * after, correctly.
     */
    template <class T = Real>
    class SABRSimModel final : public ISimulationModel<T>
    {
      public:
        SABRSimModel(T f0, T r, T alpha0, Real beta, T rho, T nu,
                     Time max_dt = 1.0 / 50.0)
            : f0_(f0), r_(r), alpha0_(alpha0), rho_(rho), nu_(nu), beta_(beta), max_dt_(max_dt)
        {
            if (to_double(f0_) <= 0.0)
                throw InvalidInput("SABRSimModel: f0 must be > 0");
            if (to_double(alpha0_) <= 0.0)
                throw InvalidInput("SABRSimModel: alpha0 must be > 0");
            if (beta_ < 0.0 || beta_ > 1.0)
                throw InvalidInput("SABRSimModel: beta must be in [0, 1]");
            if (to_double(rho_) < -1.0 || to_double(rho_) > 1.0)
                throw InvalidInput("SABRSimModel: rho must be in [-1, 1]");
            if (to_double(nu_) < 0.0)
                throw InvalidInput("SABRSimModel: nu must be >= 0");
            set_param_pointers();
        }

        /// See models/equity/bs_sim_model.hpp for why this exists.
        SABRSimModel(const SABRSimModel &other)
            : f0_(other.f0_), r_(other.r_), alpha0_(other.alpha0_), rho_(other.rho_), nu_(other.nu_), beta_(other.beta_), max_dt_(other.max_dt_), timeline_(other.timeline_), sim_timeline_(other.sim_timeline_), defline_(other.defline_), event_index_of_step_(other.event_index_of_step_), sim_dim_(other.sim_dim_)
        {
            set_param_pointers();
        }

        std::size_t n_underlyings() const override { return 1; }

        void init(const TimeLine &product_timeline,
                  const std::vector<SampleDef> &defline) override
        {
            timeline_ = canonical_timeline(product_timeline);
            sim_timeline_ = add_monitoring_steps(timeline_, max_dt_);
            defline_ = defline;

            const std::vector<std::size_t> idx =
                event_indices(sim_timeline_, timeline_);
            event_index_of_step_.assign(sim_timeline_.size(), -1);
            for (std::size_t j = 0; j < idx.size(); ++j)
                event_index_of_step_[idx[j]] = static_cast<std::ptrdiff_t>(j);

            // Every fine-grid step draws two correlated gaussians: one for
            // F's own Euler update, one (via the correlation mix) for
            // alpha's exact lognormal update.
            sim_dim_ = 2 * sim_timeline_.size();
        }

        const TimeLine &sim_timeline() const override { return sim_timeline_; }
        std::size_t sim_dim() const override { return sim_dim_; }

        void generate_path(std::span<const double> gaussians,
                           Scenario<T> &path) const override
        {
            using std::exp;
            using std::max;
            using std::pow;
            using std::sqrt;

            T F = f0_;
            T alpha = alpha0_;
            Time t_prev = 0.0;
            std::size_t g = 0;
            for (std::size_t i = 0; i < sim_timeline_.size(); ++i)
            {
                const Time t = sim_timeline_[i];
                const Time dt = t - t_prev;
                const double sqdt = sqrt(dt);

                const double z1 = gaussians[g++];
                const double z2 = gaussians[g++];
                const T dW_alpha = rho_ * z1 + sqrt(1.0 - rho_ * rho_) * z2;

                const T F_plus = max(F, 0.0);
                F = max(F + alpha * pow(F_plus, beta_) * (sqdt * z1), 0.0);
                // exact lognormal update: dalpha/alpha = nu dW2, constant nu
                alpha = alpha * exp(-0.5 * nu_ * nu_ * dt + nu_ * (sqdt * dW_alpha));
                t_prev = t;

                const std::ptrdiff_t event = event_index_of_step_[i];
                if (event < 0)
                    continue;

                Sample<T> &smp = path[static_cast<std::size_t>(event)];
                smp.spots.assign(1, F);
                smp.numeraire = exp(r_ * t);

                const SampleDef &def = defline_[static_cast<std::size_t>(event)];
                smp.discounts.resize(def.discount_mats.size());
                for (std::size_t m = 0; m < def.discount_mats.size(); ++m)
                    smp.discounts[m] = exp(-r_ * (def.discount_mats[m] - t));
            }
        }

        std::unique_ptr<ISimulationModel<T>> clone() const override
        {
            return std::make_unique<SABRSimModel<T>>(*this);
        }

        const std::vector<T *> &parameters() const override { return params_; }
        const std::vector<std::string> &parameter_labels() const override
        {
            static const std::vector<std::string> labels{"forward", "rate",
                                                         "alpha", "rho", "nu"};
            return labels;
        }

      private:
        void set_param_pointers()
        {
            params_ = {&f0_, &r_, &alpha0_, &rho_, &nu_};
        }

        T f0_{}, r_{}, alpha0_{}, rho_{}, nu_{};
        Real beta_ = 0.5;
        Time max_dt_;

        TimeLine timeline_;
        TimeLine sim_timeline_;
        std::vector<SampleDef> defline_;
        std::vector<std::ptrdiff_t> event_index_of_step_;
        std::size_t sim_dim_ = 0;
        std::vector<T *> params_;
    };

} // namespace quantModeling

#endif // QM_MODELS_EQUITY_SABR_SIM_MODEL_HPP
