#ifndef QM_ENGINES_MC_APPROX_INVERSE_CDF_HPP
#define QM_ENGINES_MC_APPROX_INVERSE_CDF_HPP

#include "quantModeling/core/sample.hpp"
#include "quantModeling/core/types.hpp"
#include "quantModeling/engines/mc/mlmc.hpp"
#include "quantModeling/instruments/simulatable.hpp"
#include "quantModeling/models/simulation_model.hpp"
#include "quantModeling/utils/inverse_normal.hpp"
#include "quantModeling/utils/rng.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <vector>

/**
 * @file approx_inverse_cdf.hpp
 * @brief The second MLMC axis of blueprint/wp/26-performance.md §8.7 (WP 26
 *        P4d): approximate random variables.
 *
 * E[P_exact] = E[P_approx] + E[P_exact - P_approx], both payoffs of the
 * correction on the same uniforms, P_approx's gaussians from a cheap inverse
 * CDF (Giles & Sheridan-Methven, ACM TOMS 49(3), 2023). Unbiased whatever
 * the approximation; it pays when the approximation is much cheaper and the
 * correction's variance small.
 *
 * Measured, not adopted: on our paths the exact inverse CDF (AS 241, P1i) is
 * a small share of a path (§8.7). This file is the experiment that measured
 * it; no route uses it.
 */
namespace quantModeling
{

    /**
     * @brief Piecewise-linear Φ⁻¹ on `cells` equal cells of p in [1/2, 1),
     *        mirrored below 1/2: the kind of approximation the papers use.
     *        The last cell (the extreme tail, probability 1/(cells+1) on each
     *        side) calls the exact Φ⁻¹, so the approximation stays bounded.
     */
    class PiecewiseLinearInverseCdf
    {
      public:
        explicit PiecewiseLinearInverseCdf(int cells)
            : cells_(cells), table_(static_cast<std::size_t>(cells) + 1)
        {
            if (cells < 2)
                throw InvalidInput("PiecewiseLinearInverseCdf: at least 2 cells");
            for (int k = 0; k <= cells; ++k)
                table_[static_cast<std::size_t>(k)] = inverse_normal_cdf(0.5 + 0.5 * k / (cells + 1.0));
        }

        int cells() const { return cells_; }

        double operator()(double p) const
        {
            const bool lower = p < 0.5;
            const double q = lower ? 1.0 - p : p;
            const double x = (q - 0.5) * 2.0 * (cells_ + 1.0);
            const int k = static_cast<int>(x);
            if (k >= cells_)
                return inverse_normal_cdf(p);
            const double w = x - k;
            const double z = table_[static_cast<std::size_t>(k)] +
                             w * (table_[static_cast<std::size_t>(k) + 1] - table_[static_cast<std::size_t>(k)]);
            return lower ? -z : z;
        }

      private:
        int cells_;
        std::vector<double> table_;
    };

    /// One level of the two-level estimator: its running sums and the wall
    /// time it took, per sample.
    struct ApproxRvLevel
    {
        MlmcLevel stats;
        double seconds = 0.0;
        double seconds_per_sample() const
        {
            return stats.n > 0 ? seconds / static_cast<double>(stats.n) : 0.0;
        }
    };

    struct ApproxRvResult
    {
        ApproxRvLevel approx;     ///< P_approx
        ApproxRvLevel correction; ///< P_exact - P_approx, same uniforms
        ApproxRvLevel exact;      ///< plain P_exact, the comparison
        double value = 0.0;       ///< approx + correction
        double std_error = 0.0;

        /// Cost of each estimator for a root-mean-square error eps, at the
        /// optimal split (Giles 2008): plain V_e C_e / eps², two-level
        /// (sqrt(V_a C_a) + sqrt(V_c C_c))² / eps²; their ratio, the gain.
        double gain() const
        {
            const double two = std::sqrt(approx.stats.variance() * approx.seconds_per_sample()) +
                               std::sqrt(correction.stats.variance() * correction.seconds_per_sample());
            return exact.stats.variance() * exact.seconds_per_sample() / (two * two);
        }
    };

    namespace approx_rv_detail
    {
        template <class Gauss>
        void run(const ISimulationModel<Real> &model, long long n,
                 std::uint64_t seed, std::uint64_t stream, Gauss &&level_sample, ApproxRvLevel &out)
        {
            Pcg32 rng(seed, stream);
            std::vector<double> u(model.sim_dim());
            const auto t0 = std::chrono::steady_clock::now();
            for (long long p = 0; p < n; ++p)
            {
                for (double &x : u)
                    x = uniform01(rng);
                const double y = level_sample(u);
                ++out.stats.n;
                out.stats.sum += y;
                out.stats.sum2 += y * y;
            }
            out.seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        }
    } // namespace approx_rv_detail

    /**
     * @brief The experiment of §8.7: n_approx samples of P_approx, n_corr of
     *        the correction, n_exact of plain P_exact (independent streams),
     *        each timed. The payoff of the product's first label.
     */
    inline ApproxRvResult simulate_approx_inverse_cdf(const ISimulatableProduct<Real> &product,
                                                      ISimulationModel<Real> &model,
                                                      const PiecewiseLinearInverseCdf &approx, long long n_approx,
                                                      long long n_corr, long long n_exact, std::uint64_t seed)
    {
        model.init(product.timeline(), product.defline());
        const std::size_t dim = model.sim_dim();
        Scenario<Real> path;
        allocate_scenario(path, product.defline(), product.n_underlyings());
        std::vector<double> z(dim);
        std::vector<Real> out;
        auto price = [&](auto &&inverse, const std::vector<double> &u)
        {
            for (std::size_t d = 0; d < dim; ++d)
                z[d] = inverse(u[d]);
            model.generate_path(z, path);
            product.payoffs(path, out);
            return out.at(0);
        };
        auto exact_inverse = [](double p)
        { return inverse_normal_cdf(p); };

        ApproxRvResult r;
        approx_rv_detail::run(model, n_approx, seed, 1, [&](const std::vector<double> &u)
                              { return price(approx, u); }, r.approx);
        approx_rv_detail::run(model, n_corr, seed, 2, [&](const std::vector<double> &u)
                              { return price(exact_inverse, u) - price(approx, u); }, r.correction);
        approx_rv_detail::run(model, n_exact, seed, 3, [&](const std::vector<double> &u)
                              { return price(exact_inverse, u); }, r.exact);
        r.value = r.approx.stats.mean() + r.correction.stats.mean();
        r.std_error = std::sqrt(r.approx.stats.variance() / static_cast<double>(n_approx) +
                                r.correction.stats.variance() / static_cast<double>(n_corr));
        return r;
    }

} // namespace quantModeling

#endif // QM_ENGINES_MC_APPROX_INVERSE_CDF_HPP
