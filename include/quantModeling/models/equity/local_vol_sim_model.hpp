#ifndef QM_MODELS_EQUITY_LOCAL_VOL_SIM_MODEL_HPP
#define QM_MODELS_EQUITY_LOCAL_VOL_SIM_MODEL_HPP

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
     * @brief Single-asset Dupire local vol for the timeline simulation
     *        architecture — the flagship AAD use case (blueprint/wp/17-aad.md
     *        §6.3): every one of the (K x T) grid's points is its own
     *        differentiable parameter, so simulate_aad reports a full vector
     *        of local vegas -- 1500 of them for a 50x30 grid -- from one run.
     *
     * Unlike Black-Scholes, local vol has no exact transition density: sigma
     * depends on the evolving spot itself, so this is a log-Euler scheme on a
     * fine internal grid (add_monitoring_steps(), a max_dt between the
     * product's own, possibly sparse, event dates) rather than one exact step
     * per event -- sim_timeline() genuinely differs from the product's own
     * timeline here, the case that interface was written for from the start.
     * A step's own sigma cannot be precomputed in init() the way BS's drift
     * can: it depends on that path's own spot, so the whole diffusion is
     * recorded inside generate_path(), per path.
     *
     * The interpolation itself mirrors models/volatility.hpp's GridLocalVol
     * exactly (same bracketing, same clamping, same flat extrapolation at the
     * edges) with one difference: the grid's K/T axes stay plain double
     * (structural coordinates, not risk factors, same as a curve's pillar
     * times), but sigma_loc's *values* are T. A lookup blends at most four
     * corners with double bilinear weights, so it records a derivative
     * exactly with respect to whichever corners were touched -- not the
     * whole grid -- for every step of every path.
     */
    template <class T = Real>
    class LocalVolSimModel final : public ISimulationModel<T>
    {
      public:
        LocalVolSimModel(T s0, T r, T q, std::vector<Real> K_grid,
                         std::vector<Real> T_grid, std::vector<T> sigma_loc,
                         Time max_dt = 1.0 / 12.0)
            : s0_(s0), r_(r), q_(q), K_grid_(std::move(K_grid)),
              T_grid_(std::move(T_grid)), sigma_loc_(std::move(sigma_loc)),
              max_dt_(max_dt)
        {
            if (K_grid_.size() < 2 || T_grid_.size() < 2)
                throw InvalidInput(
                    "LocalVolSimModel: K_grid and T_grid need at least 2 "
                    "points each");
            if (sigma_loc_.size() != K_grid_.size() * T_grid_.size())
                throw InvalidInput(
                    "LocalVolSimModel: sigma_loc size must equal "
                    "K_grid.size() * T_grid.size()");
            set_param_pointers();
        }

        /// See models/equity/bs_sim_model.hpp for why this exists: params_
        /// caches pointers into *this* object's own members, so a naive
        /// copy would leave them pointing at the original.
        LocalVolSimModel(const LocalVolSimModel &other)
            : s0_(other.s0_), r_(other.r_), q_(other.q_),
              K_grid_(other.K_grid_), T_grid_(other.T_grid_),
              sigma_loc_(other.sigma_loc_), max_dt_(other.max_dt_),
              timeline_(other.timeline_), sim_timeline_(other.sim_timeline_),
              defline_(other.defline_),
              event_index_of_step_(other.event_index_of_step_),
              sim_dim_(other.sim_dim_)
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

            // Every fine-grid step draws: add_monitoring_steps() only ever
            // returns points strictly after t = 0 (blueprint's own "draws"
            // rule -- unlike BS there is no exact-transition first point to
            // skip).
            sim_dim_ = sim_timeline_.size();
        }

        const TimeLine &sim_timeline() const override { return sim_timeline_; }
        std::size_t sim_dim() const override { return sim_dim_; }

        void generate_path(std::span<const double> gaussians,
                           Scenario<T> &path) const override
        {
            using std::exp;
            using std::sqrt;

            T S = s0_;
            Time t_prev = 0.0;
            for (std::size_t k = 0; k < sim_timeline_.size(); ++k)
            {
                const Time t = sim_timeline_[k];
                const Time dt = t - t_prev;

                const T sig = local_vol_at(to_double(S), t);
                const T drift = (r_ - q_ - 0.5 * sig * sig) * dt;
                const T vol_sqrt_dt = sig * sqrt(dt);
                S *= exp(drift + vol_sqrt_dt * gaussians[k]);
                t_prev = t;

                const std::ptrdiff_t event = event_index_of_step_[k];
                if (event < 0)
                    continue;

                Sample<T> &smp = path[static_cast<std::size_t>(event)];
                smp.spots.assign(1, S);
                smp.numeraire = exp(r_ * t);

                const SampleDef &def = defline_[static_cast<std::size_t>(event)];
                smp.discounts.resize(def.discount_mats.size());
                for (std::size_t m = 0; m < def.discount_mats.size(); ++m)
                    smp.discounts[m] = exp(-r_ * (def.discount_mats[m] - t));

                smp.forwards.resize(def.forward_mats.size());
                for (std::size_t m = 0; m < def.forward_mats.size(); ++m)
                {
                    const Time mat = def.forward_mats[m];
                    smp.forwards[m] = S * exp((r_ - q_) * (mat - t));
                }
            }
        }

        std::unique_ptr<ISimulationModel<T>> clone() const override
        {
            return std::make_unique<LocalVolSimModel<T>>(*this);
        }

        const std::vector<T *> &parameters() const override { return params_; }
        const std::vector<std::string> &parameter_labels() const override
        {
            return labels_;
        }

        const std::vector<Real> &K_grid() const { return K_grid_; }
        const std::vector<Real> &T_grid() const { return T_grid_; }

      private:
        /// Bilinear lookup, T-valued corners, double weights and clamping --
        /// same bracketing as models/volatility.hpp's GridLocalVol::value(),
        /// generalised so the four corners can be Number instead of double.
        T local_vol_at(double S, double t) const
        {
            using std::max;

            const int nK = static_cast<int>(K_grid_.size());
            const int nT = static_cast<int>(T_grid_.size());

            S = std::clamp(S, K_grid_.front(), K_grid_.back());
            t = std::clamp(t, T_grid_.front(), T_grid_.back());

            auto it_K = std::lower_bound(K_grid_.begin(), K_grid_.end(), S);
            int i1 = static_cast<int>(it_K - K_grid_.begin());
            if (i1 >= nK)
                i1 = nK - 1;
            int i0 = (i1 > 0) ? i1 - 1 : 0;
            if (i0 == i1)
            {
                if (i1 > 0)
                    i0 = i1 - 1;
                else
                    i1 = 1;
            }

            auto it_T = std::lower_bound(T_grid_.begin(), T_grid_.end(), t);
            int j1 = static_cast<int>(it_T - T_grid_.begin());
            if (j1 >= nT)
                j1 = nT - 1;
            int j0 = (j1 > 0) ? j1 - 1 : 0;
            if (j0 == j1)
            {
                if (j1 > 0)
                    j0 = j1 - 1;
                else
                    j1 = 1;
            }

            const double K0 = K_grid_[static_cast<std::size_t>(i0)];
            const double K1 = K_grid_[static_cast<std::size_t>(i1)];
            const double T0 = T_grid_[static_cast<std::size_t>(j0)];
            const double T1 = T_grid_[static_cast<std::size_t>(j1)];
            const double dK = K1 - K0, dT = T1 - T0;
            const double wK = (dK > 1e-12) ? (S - K0) / dK : 0.0;
            const double wT = (dT > 1e-12) ? (t - T0) / dT : 0.0;

            auto cell = [&](int i, int j) -> const T &
            { return sigma_loc_[static_cast<std::size_t>(i) * static_cast<std::size_t>(nT) +
                                static_cast<std::size_t>(j)]; };

            const T sigma = (1.0 - wK) * (1.0 - wT) * cell(i0, j0) +
                            wK * (1.0 - wT) * cell(i1, j0) +
                            (1.0 - wK) * wT * cell(i0, j1) +
                            wK * wT * cell(i1, j1);
            return max(sigma, 1e-6);
        }

        void set_param_pointers()
        {
            params_.clear();
            labels_.clear();
            params_.push_back(&s0_);
            labels_.push_back("spot");
            params_.push_back(&r_);
            labels_.push_back("rate");
            params_.push_back(&q_);
            labels_.push_back("div");

            const std::size_t nT = T_grid_.size();
            for (std::size_t i = 0; i < K_grid_.size(); ++i)
                for (std::size_t j = 0; j < nT; ++j)
                {
                    params_.push_back(&sigma_loc_[i * nT + j]);
                    labels_.push_back("lvol[" + std::to_string(i) + "," +
                                      std::to_string(j) + "]");
                }
        }

        T s0_{}, r_{}, q_{};
        std::vector<Real> K_grid_, T_grid_;
        std::vector<T> sigma_loc_;
        Time max_dt_;

        TimeLine timeline_;     // the product's own event dates
        TimeLine sim_timeline_; // timeline_ plus internal Euler steps
        std::vector<SampleDef> defline_;
        std::vector<std::ptrdiff_t> event_index_of_step_; // -1, or an index into defline_
        std::size_t sim_dim_ = 0;

        std::vector<T *> params_;
        std::vector<std::string> labels_;
    };

} // namespace quantModeling

#endif // QM_MODELS_EQUITY_LOCAL_VOL_SIM_MODEL_HPP
