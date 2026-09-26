#ifndef QM_MODELS_EQUITY_SLV_SIM_MODEL_HPP
#define QM_MODELS_EQUITY_SLV_SIM_MODEL_HPP

#include "quantModeling/aad/number.hpp"
#include "quantModeling/core/timegrid.hpp"
#include "quantModeling/core/types.hpp"
#include "quantModeling/models/equity/path_steps.hpp"
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
     * @brief Stochastic-local volatility (Heston dynamics, Dupire-exact
     *        marginals) for the timeline simulation architecture -- issue
     *        #37's SLV item, priced/differentiated with a *pre-calibrated*
     *        leverage grid (market/slv_calibration.hpp).
     *
     *   dS/S = (r - q) dt + L(S,t) sqrt(v) dW1
     *   dv   = kappa (theta - v) dt + xi sqrt(v) dW2,   dW1 dW2 = rho dt
     *
     * This is exactly BatesSimModel's Heston piece (lambda = 0: same full-
     * truncation Euler scheme for v, Lord-Koekkoek-van Dijk, same variance
     * floor and the same reason for it -- aad::sqrt's adjoint is +inf at 0)
     * with the spot's diffusion coefficient sqrt(v) replaced by L(S,t)
     * sqrt(v), and L looked up from a (K,T) grid exactly the way
     * LocalVolSimModel looks up sigma_loc: same bilinear bracketing, same
     * flat extrapolation at the edges, K/T axes staying double (structural
     * grid coordinates) while the grid's *values* are T -- so simulate_aad
     * reports a risk per leverage grid point alongside v0/kappa/theta/xi/
     * rho, the same "local vega" flagship use case §6.3 describes for pure
     * local vol, now on the leverage surface instead.
     *
     * The leverage grid itself is calibrated once, offline, by
     * market/slv_calibration.hpp's particle method (Real-only -- the
     * calibration does not need AAD, only the pricing/risk run does). This
     * model does not calibrate anything: it takes L(K,T) as data, same
     * relationship LocalVolSimModel has to Dupire's own calibration.
     */
    template <class T = Real>
    class SLVSimModel final : public ISimulationModel<T>
    {
      public:
        SLVSimModel(T s0, T r, T q, T v0, T kappa, T theta, T xi, T rho,
                    std::vector<Real> K_grid, std::vector<Real> T_grid,
                    std::vector<T> leverage, Time max_dt = 1.0 / 50.0)
            : s0_(s0), r_(r), q_(q), v0_(v0), kappa_(kappa), theta_(theta), xi_(xi), rho_(rho), K_grid_(std::move(K_grid)), T_grid_(std::move(T_grid)), leverage_(std::move(leverage)), max_dt_(max_dt)
        {
            if (to_double(v0_) < 0.0)
                throw InvalidInput("SLVSimModel: v0 must be >= 0");
            if (to_double(kappa_) <= 0.0)
                throw InvalidInput("SLVSimModel: kappa must be > 0");
            if (to_double(theta_) <= 0.0)
                throw InvalidInput("SLVSimModel: theta must be > 0");
            if (to_double(xi_) <= 0.0)
                throw InvalidInput("SLVSimModel: xi must be > 0");
            if (to_double(rho_) < -1.0 || to_double(rho_) > 1.0)
                throw InvalidInput("SLVSimModel: rho must be in [-1, 1]");
            if (K_grid_.size() < 2 || T_grid_.size() < 2)
                throw InvalidInput("SLVSimModel: K_grid and T_grid need at least 2 points each");
            if (leverage_.size() != K_grid_.size() * T_grid_.size())
                throw InvalidInput("SLVSimModel: leverage size must equal K_grid.size() * T_grid.size()");
            set_param_pointers();
        }

        /// See models/equity/bs_sim_model.hpp for why this exists.
        SLVSimModel(const SLVSimModel &other)
            : s0_(other.s0_), r_(other.r_), q_(other.q_), v0_(other.v0_), kappa_(other.kappa_), theta_(other.theta_), xi_(other.xi_), rho_(other.rho_), K_grid_(other.K_grid_), T_grid_(other.T_grid_), leverage_(other.leverage_), max_dt_(other.max_dt_), timeline_(other.timeline_), sim_timeline_(other.sim_timeline_), defline_(other.defline_), event_index_of_step_(other.event_index_of_step_), sim_dim_(other.sim_dim_)
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

            const std::vector<std::size_t> idx = event_indices(sim_timeline_, timeline_);
            event_index_of_step_.assign(sim_timeline_.size(), -1);
            for (std::size_t j = 0; j < idx.size(); ++j)
                event_index_of_step_[idx[j]] = static_cast<std::ptrdiff_t>(j);

            // Every fine-grid step draws two correlated normals (spot,
            // variance) -- same shape as BatesSimModel without the two
            // jump draws.
            sim_dim_ = 2 * sim_timeline_.size();
        }

        const TimeLine &sim_timeline() const override { return sim_timeline_; }
        std::size_t sim_dim() const override { return sim_dim_; }

        /// Every fine-grid step draws: spot and the variance shock's independent part.
        BrownianLayout brownian_layout() const override
        {
            return BrownianLayout{sim_timeline_, 2, 2};
        }

        void generate_path(std::span<const double> gaussians,
                           Scenario<T> &path) const override
        {
            using std::exp;
            using std::max;
            using std::sqrt;

            T S = s0_;
            T v = v0_;
            Time t_prev = 0.0;
            std::size_t g = 0;
            for (std::size_t i = 0; i < sim_timeline_.size(); ++i)
            {
                const Time t = sim_timeline_[i];
                const Time dt = t - t_prev;
                const double z_spot = gaussians[g++];
                const double z_indep = gaussians[g++];
                mc::slv_step(leverage_grid(), heston(), r_, q_, S, v, t, dt, z_spot, z_indep);
                t_prev = t;

                const std::ptrdiff_t event = event_index_of_step_[i];
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
            return std::make_unique<SLVSimModel<T>>(*this);
        }

        const std::vector<T *> &parameters() const override { return params_; }
        const std::vector<std::string> &parameter_labels() const override { return labels_; }

        const std::vector<Real> &K_grid() const { return K_grid_; }
        const std::vector<Real> &T_grid() const { return T_grid_; }

      private:
        static constexpr double kVarianceFloor = 1e-10;

        /// Bilinear in K, but *staircase* in T -- not the smooth bilinear
        /// bracketing LocalVolSimModel::local_vol_at() uses. This has to
        /// match market/slv_calibration.hpp's own convention exactly, not
        /// just look plausible: column j there was calibrated by stepping
        /// particles *from* column j-1's leverage held constant across the
        /// whole interval (T_grid[j-1], T_grid[j]], and reading off the
        /// resulting distribution at T_grid[j] -- so column j-1, not a
        /// blend of j-1 and j, is what is actually valid on that interval.
        /// Smoothly blending toward column j partway through the interval
        /// was tried first and measurably biased option prices (caught by
        /// SLVSimModel.CalibratedToFlatVolReproducesBlackScholes, off by
        /// ~6% at one strike -- more than Monte Carlo noise could explain).
        /// t <= T_grid.front() uses column 0 itself: the calibration's
        /// bootstrap step uses a single scalar leverage (every particle
        /// starts exactly at s0, so there is no S-dependence to speak of
        /// yet), which column 0 already approximates closely for a short
        /// first interval, and is not otherwise stored anywhere to look up.
        T leverage_at(const T &S_t, double t) const
        {
            return mc::leverage_staircase(leverage_grid(), S_t, t);
        }

        mc::GridView<T> leverage_grid() const
        {
            return {K_grid_.data(), static_cast<int>(K_grid_.size()), T_grid_.data(),
                    static_cast<int>(T_grid_.size()), leverage_.data()};
        }

        mc::HestonParamsT<T> heston() const { return {v0_, kappa_, theta_, xi_, rho_}; }

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
            params_.push_back(&v0_);
            labels_.push_back("v0");
            params_.push_back(&kappa_);
            labels_.push_back("kappa");
            params_.push_back(&theta_);
            labels_.push_back("theta");
            params_.push_back(&xi_);
            labels_.push_back("xi");
            params_.push_back(&rho_);
            labels_.push_back("rho");

            const std::size_t nT = T_grid_.size();
            for (std::size_t i = 0; i < K_grid_.size(); ++i)
                for (std::size_t j = 0; j < nT; ++j)
                {
                    params_.push_back(&leverage_[i * nT + j]);
                    labels_.push_back("leverage[" + std::to_string(i) + "," + std::to_string(j) + "]");
                }
        }

        T s0_{}, r_{}, q_{}, v0_{}, kappa_{}, theta_{}, xi_{}, rho_{};
        std::vector<Real> K_grid_, T_grid_;
        std::vector<T> leverage_;
        Time max_dt_;

        TimeLine timeline_;
        TimeLine sim_timeline_;
        std::vector<SampleDef> defline_;
        std::vector<std::ptrdiff_t> event_index_of_step_;
        std::size_t sim_dim_ = 0;

        std::vector<T *> params_;
        std::vector<std::string> labels_;
    };

} // namespace quantModeling

#endif // QM_MODELS_EQUITY_SLV_SIM_MODEL_HPP
