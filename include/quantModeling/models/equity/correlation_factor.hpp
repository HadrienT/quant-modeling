#ifndef QM_MODELS_EQUITY_CORRELATION_FACTOR_HPP
#define QM_MODELS_EQUITY_CORRELATION_FACTOR_HPP

#include "quantModeling/core/types.hpp"

#include <Eigen/Cholesky>
#include <Eigen/Core>
#include <Eigen/Eigenvalues>
#include <string>

namespace quantModeling
{

    /**
     * A mixing matrix A with A A^T = corr: independent gaussians u become
     * correlated shocks z = A u. The Cholesky factor when corr is positive
     * definite; the spectral square root when it is only positive
     * *semi*-definite (e.g. a rho = 1 block, which has no strict Cholesky
     * factor). Throws InvalidInput, naming `who`, otherwise.
     */
    inline Eigen::MatrixXd correlation_factor(const Eigen::MatrixXd &corr, const std::string &who)
    {
        const Eigen::LLT<Eigen::MatrixXd> llt(corr);
        if (llt.info() == Eigen::Success)
            return llt.matrixL();
        const Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(corr);
        if (es.info() != Eigen::Success || es.eigenvalues().minCoeff() < -1e-10 * corr.rows())
            throw InvalidInput(who + ": correlation matrix is not positive semi-definite");
        return es.eigenvectors() * es.eigenvalues().cwiseMax(0.0).cwiseSqrt().asDiagonal();
    }

} // namespace quantModeling

#endif // QM_MODELS_EQUITY_CORRELATION_FACTOR_HPP
