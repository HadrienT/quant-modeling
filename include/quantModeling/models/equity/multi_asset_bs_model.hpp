#ifndef EQUITY_MULTI_ASSET_BS_MODEL_HPP
#define EQUITY_MULTI_ASSET_BS_MODEL_HPP

#include "quantModeling/core/types.hpp"
#include "quantModeling/models/base.hpp"

#include <Eigen/Cholesky>
#include <Eigen/Core>
#include <string>
#include <vector>

namespace quantModeling
{

    /**
     * @brief Multi-asset Black-Scholes model for basket options.
     *
     * Each asset i follows GBM:
     *   dS_i = (r - q_i) S_i dt + sigma_i S_i dW_i
     *   dW_i dW_j = rho_ij dt
     *
     * The Cholesky factor L (s.t. L L^T = C, lower-triangular) is computed
     * at construction time from the user-supplied correlation matrix C.
     * The engine draws i.i.d. N(0,1) vector u and computes z = L * u, giving
     * the correlated Gaussians needed to simulate the terminal prices.
     */
    struct MultiAssetBSModel final : public IModel
    {
        Real rate_r;                 ///< single risk-free rate
        std::vector<Real> spots;     ///< S0_i  (n)
        std::vector<Real> vols;      ///< sigma_i (n)
        std::vector<Real> dividends; ///< q_i   (n)
        Eigen::MatrixXd chol;        ///< lower-triangular L s.t. L L^T = corr

        /**
         * @param r   Risk-free rate
         * @param s   Initial spots S0_i
         * @param v   Volatilities sigma_i
         * @param q   Dividend yields q_i
         * @param corr  n×n correlation matrix (must be positive definite)
         * @throws InvalidInput if corr is not positive definite
         */
        MultiAssetBSModel(Real r,
                          std::vector<Real> s,
                          std::vector<Real> v,
                          std::vector<Real> q,
                          const Eigen::MatrixXd &corr)
            : rate_r(r),
              spots(std::move(s)),
              vols(std::move(v)),
              dividends(std::move(q))
        {
            const Eigen::LLT<Eigen::MatrixXd> llt(corr);
            if (llt.info() != Eigen::Success)
                throw InvalidInput("MultiAssetBSModel: correlation matrix is not positive definite");
            chol = llt.matrixL();
        }

        std::string model_name() const noexcept override { return "MultiAssetBSModel"; }

        int n_assets() const { return static_cast<int>(spots.size()); }
    };

} // namespace quantModeling

#endif
