#ifndef QM_MODELS_EQUITY_MULTI_ASSET_LOCAL_VOL_SIM_MODEL_HPP
#define QM_MODELS_EQUITY_MULTI_ASSET_LOCAL_VOL_SIM_MODEL_HPP

#include "quantModeling/aad/number.hpp"
#include "quantModeling/core/timegrid.hpp"
#include "quantModeling/core/types.hpp"
#include "quantModeling/models/equity/correlation_factor.hpp"
#include "quantModeling/models/equity/path_steps.hpp"
#include "quantModeling/models/simulation_model.hpp"

#include <Eigen/Core>
#include <cmath>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace quantModeling
{

    /**
     * @brief n underlyings, each on its own Dupire surface, correlated
     *        through their Brownian drivers -- the multi-asset local vol of a
     *        desk's worst-ofs and baskets (issue #86):
     *
     *   dS_i = (r - q_i) S_i dt + sigma_i(S_i, t) S_i dW_i,   dW_i dW_j = rho_ij dt.
     *
     * Each marginal is its own surface's (the single-asset LocalVolSimModel,
     * step for step: mc::local_vol_step on its grid), so every asset
     * reprices its own vanillas; the dependence is the constant correlation
     * of the drivers, the standard choice when no correlation product is
     * quoted to calibrate anything richer (a local correlation needs
     * basket or index options). One log-Euler step per internal grid point
     * (add_monitoring_steps with max_dt), n draws per step, mixed by the
     * correlation factor like MultiAssetBSSimModel's.
     *
     * CPU only: describe_device declines, and the script engine says so.
     * Every grid value is a parameter (labels "lvol[i:a,b]", asset i, grid
     * point (a, b)), so simulate_aad reports each asset's local vegas.
     */
    template <class T = Real>
    class MultiAssetLocalVolSimModel final : public ISimulationModel<T>
    {
      public:
        struct Surface
        {
            std::vector<Real> K_grid;
            std::vector<Real> T_grid;
            std::vector<T> sigma_loc; ///< K-major
        };

        MultiAssetLocalVolSimModel(std::vector<T> s0, T r, std::vector<T> q,
                                   std::vector<Surface> surfaces, const Eigen::MatrixXd &corr,
                                   Time max_dt = 1.0 / 52.0)
            : s0_(std::move(s0)), r_(r), q_(std::move(q)), surfaces_(std::move(surfaces)), max_dt_(max_dt)
        {
            const std::size_t n = s0_.size();
            if (n == 0 || q_.size() != n || surfaces_.size() != n)
                throw InvalidInput("MultiAssetLocalVolSimModel: one spot, dividend and surface per asset");
            if (static_cast<std::size_t>(corr.rows()) != n || corr.rows() != corr.cols())
                throw InvalidInput("MultiAssetLocalVolSimModel: correlation matrix size must match the number of "
                                   "assets");
            for (const Surface &s : surfaces_)
            {
                if (s.K_grid.size() < 2 || s.T_grid.size() < 2)
                    throw InvalidInput("MultiAssetLocalVolSimModel: every grid needs at least 2 strikes and 2 "
                                       "maturities");
                if (s.sigma_loc.size() != s.K_grid.size() * s.T_grid.size())
                    throw InvalidInput("MultiAssetLocalVolSimModel: sigma_loc size must equal K_grid.size() * "
                                       "T_grid.size()");
            }
            chol_ = correlation_factor(corr, "MultiAssetLocalVolSimModel");
            set_param_pointers();
        }

        /// params_ points into this object's members: a copy re-points them.
        MultiAssetLocalVolSimModel(const MultiAssetLocalVolSimModel &other)
            : s0_(other.s0_), r_(other.r_), q_(other.q_), surfaces_(other.surfaces_), max_dt_(other.max_dt_), chol_(other.chol_), timeline_(other.timeline_), sim_timeline_(other.sim_timeline_), defline_(other.defline_), event_index_of_step_(other.event_index_of_step_)
        {
            set_param_pointers();
        }

        std::size_t n_underlyings() const override { return s0_.size(); }

        void init(const TimeLine &product_timeline, const std::vector<SampleDef> &defline) override
        {
            timeline_ = canonical_timeline(product_timeline);
            sim_timeline_ = add_monitoring_steps(timeline_, max_dt_);
            defline_ = defline;
            const std::vector<std::size_t> idx = event_indices(sim_timeline_, timeline_);
            event_index_of_step_.assign(sim_timeline_.size(), -1);
            for (std::size_t j = 0; j < idx.size(); ++j)
                event_index_of_step_[idx[j]] = static_cast<std::ptrdiff_t>(j);
        }

        const TimeLine &sim_timeline() const override { return sim_timeline_; }
        std::size_t sim_dim() const override { return sim_timeline_.size() * s0_.size(); }

        BrownianLayout brownian_layout() const override
        {
            return BrownianLayout{sim_timeline_, s0_.size(), s0_.size()};
        }

        void generate_path(std::span<const double> gaussians, Scenario<T> &path) const override
        {
            using std::exp;

            const std::size_t n = s0_.size();
            std::vector<T> S = s0_;
            std::vector<double> z(n);
            Time t_prev = 0.0;
            std::size_t g = 0;
            for (std::size_t k = 0; k < sim_timeline_.size(); ++k)
            {
                const Time t = sim_timeline_[k];
                const Time dt = t - t_prev;
                // z = chol_ u: the full row, since the spectral fallback is dense.
                for (std::size_t a = 0; a < n; ++a)
                {
                    double acc = 0.0;
                    for (std::size_t c = 0; c < n; ++c)
                        acc += chol_(static_cast<Eigen::Index>(a), static_cast<Eigen::Index>(c)) * gaussians[g + c];
                    z[a] = acc;
                }
                g += n;
                for (std::size_t a = 0; a < n; ++a)
                    mc::local_vol_step(grid(a), r_, q_[a], S[a], t, dt, z[a]);
                t_prev = t;

                const std::ptrdiff_t event = event_index_of_step_[k];
                if (event < 0)
                    continue;
                Sample<T> &smp = path[static_cast<std::size_t>(event)];
                smp.spots = S;
                smp.numeraire = exp(r_ * t);
                // Indexed forwards and discounts have no script syntax on
                // several assets (see MultiAssetBSSimModel): always empty.
                const SampleDef &def = defline_[static_cast<std::size_t>(event)];
                smp.discounts.resize(def.discount_mats.size());
                smp.forwards.resize(def.forward_mats.size());
            }
        }

        std::unique_ptr<ISimulationModel<T>> clone() const override
        {
            return std::make_unique<MultiAssetLocalVolSimModel<T>>(*this);
        }

        /// Log-Euler is exact in mean: S_i(t) / N(t) is a martingale on the
        /// grid, E = S_i(0) e^{-q_i t}.
        bool deflated_spot_mean(std::size_t asset, Time t, Real &mean) const override
        {
            if (asset >= s0_.size())
                return false;
            mean = to_double(s0_[asset]) * std::exp(-to_double(q_[asset]) * t);
            return true;
        }

        const std::vector<T *> &parameters() const override { return params_; }
        const std::vector<std::string> &parameter_labels() const override { return labels_; }

      private:
        mc::GridView<T> grid(std::size_t a) const
        {
            const Surface &s = surfaces_[a];
            return {s.K_grid.data(), static_cast<int>(s.K_grid.size()), s.T_grid.data(),
                    static_cast<int>(s.T_grid.size()), s.sigma_loc.data()};
        }

        void set_param_pointers()
        {
            params_.clear();
            labels_.clear();
            params_.push_back(&r_);
            labels_.push_back("rate");
            for (std::size_t i = 0; i < s0_.size(); ++i)
            {
                params_.push_back(&s0_[i]);
                labels_.push_back("spot[" + std::to_string(i) + "]");
            }
            for (std::size_t i = 0; i < q_.size(); ++i)
            {
                params_.push_back(&q_[i]);
                labels_.push_back("div[" + std::to_string(i) + "]");
            }
            for (std::size_t i = 0; i < surfaces_.size(); ++i)
            {
                Surface &s = surfaces_[i];
                const std::size_t nT = s.T_grid.size();
                for (std::size_t a = 0; a < s.K_grid.size(); ++a)
                    for (std::size_t b = 0; b < nT; ++b)
                    {
                        params_.push_back(&s.sigma_loc[a * nT + b]);
                        labels_.push_back("lvol[" + std::to_string(i) + ":" + std::to_string(a) + "," +
                                          std::to_string(b) + "]");
                    }
            }
        }

        std::vector<T> s0_;
        T r_{};
        std::vector<T> q_;
        std::vector<Surface> surfaces_;
        Time max_dt_;
        Eigen::MatrixXd chol_;

        TimeLine timeline_;
        TimeLine sim_timeline_;
        std::vector<SampleDef> defline_;
        std::vector<std::ptrdiff_t> event_index_of_step_;
        std::vector<T *> params_;
        std::vector<std::string> labels_;
    };

} // namespace quantModeling

#endif // QM_MODELS_EQUITY_MULTI_ASSET_LOCAL_VOL_SIM_MODEL_HPP
