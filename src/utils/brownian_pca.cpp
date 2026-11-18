#include "quantModeling/utils/brownian_bridge.hpp"

#include <Eigen/Eigenvalues>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace quantModeling
{

    std::vector<Real> brownian_pca_matrix(std::span<const Time> times)
    {
        const Eigen::Index n = static_cast<Eigen::Index>(times.size());
        if (n == 0)
            throw InvalidInput("brownian_pca_matrix: empty time grid");
        for (Eigen::Index i = 0; i < n; ++i)
            if (times[static_cast<std::size_t>(i)] <= (i == 0 ? 0.0 : times[static_cast<std::size_t>(i) - 1]))
                throw InvalidInput("brownian_pca_matrix: times must be strictly increasing and > 0");
        // Cov(W(t_i), W(t_j)) = min(t_i, t_j)
        Eigen::MatrixXd C(n, n);
        for (Eigen::Index i = 0; i < n; ++i)
            for (Eigen::Index j = 0; j < n; ++j)
                C(i, j) = std::min(times[static_cast<std::size_t>(i)], times[static_cast<std::size_t>(j)]);
        const Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> eig(C);
        if (eig.info() != Eigen::Success)
            throw InvalidInput("brownian_pca_matrix: the eigen-decomposition failed");
        // Eigen sorts ascending: column k of the matrix is component n-1-k,
        // so that variate 0 drives the largest eigenvalue.
        std::vector<Real> a(static_cast<std::size_t>(n * n));
        for (Eigen::Index k = 0; k < n; ++k)
        {
            const Eigen::Index src = n - 1 - k;
            const double scale = std::sqrt(std::max(0.0, eig.eigenvalues()(src)));
            // a fixed sign per component: the same path for the same point,
            // whatever Eigen's solver returns
            double sign = 1.0;
            for (Eigen::Index s = n - 1; s >= 0; --s)
                if (std::abs(eig.eigenvectors()(s, src)) > 1e-12)
                {
                    sign = eig.eigenvectors()(s, src) > 0.0 ? 1.0 : -1.0;
                    break;
                }
            for (Eigen::Index s = 0; s < n; ++s)
                a[static_cast<std::size_t>(s * n + k)] = sign * scale * eig.eigenvectors()(s, src);
        }
        return a;
    }

} // namespace quantModeling
