#ifndef QM_MODELS_EQUITY_MULTI_ASSET_BS_SIM_MODEL_HPP
#define QM_MODELS_EQUITY_MULTI_ASSET_BS_SIM_MODEL_HPP

#include "quantModeling/aad/number.hpp"
#include "quantModeling/core/types.hpp"
#include "quantModeling/models/simulation_model.hpp"

#include <Eigen/Cholesky>
#include <Eigen/Core>
#include <Eigen/Eigenvalues>
#include <cmath>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace quantModeling
{

    /**
     * @brief Multi-asset Black-Scholes for the timeline simulation
     *        architecture — n correlated underlyings, one shared rate.
     *
     * Each asset i follows GBM under one money-market measure:
     *   dS_i = (r - q_i) S_i dt + sigma_i S_i dW_i,   dW_i dW_j = rho_ij dt
     *
     * The correlation matrix is factored once at construction (Cholesky, or
     * a spectral square root when it is only positive *semi*-definite --
     * same fallback as the existing, non-templated MultiAssetBSModel this
     * mirrors) into a mixing matrix `chol_` with `chol_ chol_^T = corr`.
     * Independent per-asset gaussians `u` become correlated shocks
     * `z = chol_ * u`. `chol_` stays a plain `Eigen::MatrixXd` -- it mixes
     * *draws* (always double, blueprint §6.1), never a T -- so correlation
     * risk is explicitly out of scope for v1 here, same as the book's own
     * sequencing (blueprint §6.3: "corrélation en double en v1").
     *
     * s0_/q_/sigma_ are one T per asset; r_ is a single shared T. Labels use
     * the project's own bracket convention for an indexed parameter (see
     * blueprint §13.1's "lvol[12,3]" example): "spot[i]", "div[i]", "vol[i]",
     * and the unindexed "rate".
     */
    template <class T = Real>
    class MultiAssetBSSimModel final : public ISimulationModel<T>
    {
      public:
        MultiAssetBSSimModel(std::vector<T> s0, T r, std::vector<T> q,
                             std::vector<T> sigma, const Eigen::MatrixXd &corr)
            : s0_(std::move(s0)), r_(r), q_(std::move(q)),
              sigma_(std::move(sigma))
        {
            if (s0_.size() != q_.size() || s0_.size() != sigma_.size())
                throw InvalidInput(
                    "MultiAssetBSSimModel: spot, dividend and vol vectors "
                    "must have the same length");
            if (s0_.empty())
                throw InvalidInput("MultiAssetBSSimModel: need at least one asset");
            if (static_cast<std::size_t>(corr.rows()) != s0_.size() ||
                corr.rows() != corr.cols())
                throw InvalidInput(
                    "MultiAssetBSSimModel: correlation matrix size must "
                    "match the number of assets");

            factor_correlation(corr);
            set_param_pointers();
        }

        /// See models/equity/bs_sim_model.hpp for why this exists: params_
        /// caches pointers into *this* object's own members, so a naive
        /// copy would leave them pointing at the original.
        MultiAssetBSSimModel(const MultiAssetBSSimModel &other)
            : s0_(other.s0_), r_(other.r_), q_(other.q_), sigma_(other.sigma_),
              chol_(other.chol_), timeline_(other.timeline_),
              defline_(other.defline_), steps_(other.steps_),
              sim_dim_(other.sim_dim_)
        {
            set_param_pointers();
        }

        std::size_t n_underlyings() const override { return s0_.size(); }
        int n_assets() const { return static_cast<int>(s0_.size()); }

        void init(const TimeLine &product_timeline,
                  const std::vector<SampleDef> &defline) override
        {
            using std::sqrt;

            timeline_ = canonical_timeline(product_timeline);
            defline_ = defline;

            const std::size_t n = s0_.size();
            std::vector<T> mu(n);
            for (std::size_t i = 0; i < n; ++i)
                mu[i] = r_ - q_[i] - 0.5 * sigma_[i] * sigma_[i];

            steps_.clear();
            sim_dim_ = 0;
            Time t_prev = 0.0;
            for (const Time t : timeline_)
            {
                Step s;
                s.t = t;
                s.draws = (t > TIMELINE_EPS);
                if (s.draws)
                {
                    const Time dt = t - t_prev;
                    s.drift.resize(n);
                    s.vol_sqrt_dt.resize(n);
                    for (std::size_t i = 0; i < n; ++i)
                    {
                        s.drift[i] = mu[i] * dt;
                        s.vol_sqrt_dt[i] = sigma_[i] * sqrt(dt);
                    }
                    sim_dim_ += n;
                }
                steps_.push_back(std::move(s));
                t_prev = t;
            }
        }

        const TimeLine &sim_timeline() const override { return timeline_; }
        std::size_t sim_dim() const override { return sim_dim_; }

        void generate_path(std::span<const double> gaussians,
                           Scenario<T> &path) const override
        {
            using std::exp;

            const std::size_t n = s0_.size();
            std::vector<T> S = s0_;
            std::vector<double> u(n), z(n);

            std::size_t g = 0;
            for (std::size_t i = 0; i < steps_.size(); ++i)
            {
                const Step &st = steps_[i];
                if (st.draws)
                {
                    for (std::size_t k = 0; k < n; ++k)
                        u[k] = gaussians[g++];
                    // z = chol_ * u -- correlated shocks, still pure double:
                    // the mix is over *draws*, never over a T parameter.
                    // Full row, not just the lower triangle: the spectral
                    // fallback in factor_correlation() produces a dense
                    // matrix, not a triangular one, when the correlation is
                    // only positive *semi*-definite.
                    for (std::size_t r = 0; r < n; ++r)
                    {
                        double acc = 0.0;
                        for (std::size_t c = 0; c < n; ++c)
                            acc += chol_(static_cast<Eigen::Index>(r),
                                        static_cast<Eigen::Index>(c)) *
                                  u[c];
                        z[r] = acc;
                    }
                    for (std::size_t k = 0; k < n; ++k)
                        S[k] *= exp(st.drift[k] + st.vol_sqrt_dt[k] * z[k]);
                }

                Sample<T> &smp = path[i];
                smp.spots = S;
                smp.numeraire = exp(r_ * st.t);
                // Forward/discount requests indexed by asset have no script
                // syntax yet (spot() is still single-asset at the language
                // level) -- def.discount_mats/forward_mats stay empty for
                // every multi-asset product today, so there is nothing
                // asset-ambiguous to resolve here.
                // resize(), not assign(n, T(0)): the latter would construct
                // a T(0) leaf on the tape even when n == 0 (a script cannot
                // request an indexed forward/discount yet, so it always is)
                // -- exactly the wasted-leaf trap closed in
                // instruments/equity/simulatable_asian.hpp for the same
                // reason. resize() default-constructs each T, which for
                // aad::Number is a zero-initialized non-leaf (no node).
                const SampleDef &def = defline_[i];
                smp.discounts.resize(def.discount_mats.size());
                smp.forwards.resize(def.forward_mats.size());
            }
        }

        std::unique_ptr<ISimulationModel<T>> clone() const override
        {
            return std::make_unique<MultiAssetBSSimModel<T>>(*this);
        }

        const std::vector<T *> &parameters() const override { return params_; }
        const std::vector<std::string> &parameter_labels() const override
        {
            return labels_;
        }

      private:
        struct Step
        {
            Time t = 0.0;
            bool draws = false;
            std::vector<T> drift;
            std::vector<T> vol_sqrt_dt;
        };

        void factor_correlation(const Eigen::MatrixXd &corr)
        {
            const Eigen::LLT<Eigen::MatrixXd> llt(corr);
            if (llt.info() == Eigen::Success)
            {
                chol_ = llt.matrixL();
                return;
            }
            // Positive *semi*-definite (e.g. a rho = 1 block) has no strict
            // Cholesky factor; any A with A A^T = C works as a mixing
            // matrix, so fall back to the spectral square root.
            const Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(corr);
            if (es.info() != Eigen::Success ||
                es.eigenvalues().minCoeff() < -1e-10 * corr.rows())
                throw InvalidInput(
                    "MultiAssetBSSimModel: correlation matrix is not "
                    "positive semi-definite");
            chol_ = es.eigenvectors() *
                    es.eigenvalues().cwiseMax(0.0).cwiseSqrt().asDiagonal();
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
            for (std::size_t i = 0; i < sigma_.size(); ++i)
            {
                params_.push_back(&sigma_[i]);
                labels_.push_back("vol[" + std::to_string(i) + "]");
            }
        }

        std::vector<T> s0_;
        T r_{};
        std::vector<T> q_, sigma_;
        Eigen::MatrixXd chol_;
        TimeLine timeline_;
        std::vector<SampleDef> defline_;
        std::vector<Step> steps_;
        std::size_t sim_dim_ = 0;
        std::vector<T *> params_;
        std::vector<std::string> labels_;
    };

} // namespace quantModeling

#endif // QM_MODELS_EQUITY_MULTI_ASSET_BS_SIM_MODEL_HPP
