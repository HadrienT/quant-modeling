#ifndef QM_ENGINES_MC_MLQMC_HPP
#define QM_ENGINES_MC_MLQMC_HPP

#include "quantModeling/engines/mc/mlmc.hpp"
#include "quantModeling/utils/brownian_bridge.hpp"
#include "quantModeling/utils/sobol.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

/**
 * @file mlqmc.hpp
 * @brief Multilevel quasi-Monte Carlo (blueprint/wp/26-performance.md §9,
 *        WP 26 P5; Giles & Waterhouse 2009).
 *
 * The levels and their coupling are P4's (mlmc.hpp). Each level's draws are
 * points of R independently randomised Sobol sequences of the fine level's
 * dimension, the coordinates in importance order through a Brownian bridge
 * on the fine grid (BridgedGaussians, from the model's brownian_layout()),
 * the coarse increments summed from the fine ones as in P4. A level's
 * estimate is the mean of its R replicate means, its variance their sample
 * variance over R. N_l, the points per replicate, starts at a power of two
 * and doubles on the level whose variance per unit of cost falls most:
 * largest V_l / (C_l N_l) (Giles & Waterhouse 2009, §3), until sum V_l <=
 * eps^2 / 2; the bias test and the levels added are P4's.
 */
namespace quantModeling
{

    struct MlqmcSettings
    {
        double eps = 0.0; ///< target root-mean-square error, absolute (> 0)
        int min_finest_level = 2;
        int max_finest_level = 10;
        int replicates = 32;           ///< R
        long long initial_points = 16; ///< per replicate, a power of two
        std::uint64_t seed = 1;
        bool owen = false; ///< Owen's hash-based scrambling instead of the digital shift
    };

    namespace mlqmc_detail
    {
        /// One level's R Sobol replicates, their running sums, and its
        /// sampler.
        class QmcLevel
        {
          public:
            QmcLevel(const ISimulatableProduct<Real> &product, const ISimulationModel<Real> &model, int level,
                     const MlqmcSettings &s)
                : sampler_(product, model, level, s.seed), sums_(static_cast<std::size_t>(s.replicates), 0.0)
            {
                const std::size_t dim = sampler_.draws().size();
                if (dim > static_cast<std::size_t>(sobol_detail::kMaxDimension))
                    throw InvalidInput("multilevel QMC: level " + std::to_string(level) + " needs " +
                                       std::to_string(dim) + " draws per path, more than the " +
                                       std::to_string(sobol_detail::kMaxDimension) + " Sobol dimensions");
                for (int r = 0; r < s.replicates; ++r)
                {
                    // Independent across levels and replicates.
                    const std::uint64_t key = (s.seed << 32) ^ (static_cast<std::uint64_t>(level) << 16) ^
                                              static_cast<std::uint64_t>(r);
                    seq_.emplace_back(static_cast<int>(dim), key, s.owen);
                }
                const BrownianLayout layout = sampler_.fine_layout();
                if (layout.covers(dim))
                    bridge_.emplace(layout.times, layout.factors, layout.stride);
                point_.resize(dim);
                cost_ = sampler_.cost();
            }

            /// `n` more points on every replicate.
            void add(long long n)
            {
                for (std::size_t r = 0; r < seq_.size(); ++r)
                    for (long long p = 0; p < n; ++p)
                    {
                        seq_[r].next_gaussian(point_);
                        if (bridge_)
                            bridge_->map(point_, sampler_.draws());
                        else
                            std::copy(point_.begin(), point_.end(), sampler_.draws().begin());
                        sums_[r] += sampler_.evaluate();
                    }
                points_ += n;
            }

            long long points() const { return points_; }
            double cost() const { return cost_; }

            double mean() const
            {
                double m = 0.0;
                for (const double v : sums_)
                    m += v;
                return m / (static_cast<double>(sums_.size()) * static_cast<double>(points_));
            }

            /// Variance of the level's estimate: the replicate means' sample
            /// variance over R.
            double estimator_variance() const
            {
                const double m = mean(), R = static_cast<double>(sums_.size());
                double v = 0.0;
                for (const double s : sums_)
                {
                    const double d = s / static_cast<double>(points_) - m;
                    v += d * d;
                }
                return v / (R - 1.0) / R;
            }

          private:
            mlmc_detail::LevelSampler sampler_;
            std::vector<SobolSequence> seq_;
            std::optional<BridgedGaussians> bridge_;
            std::vector<double> sums_;
            std::vector<double> point_;
            long long points_ = 0;
            double cost_ = 0.0;
        };

        /// alpha over the levels from 1 whose mean is resolved (|mean| > 2
        /// standard errors of the level's estimate), as mlmc_detail.
        inline double alpha_estimate(const std::vector<std::unique_ptr<QmcLevel>> &levels)
        {
            std::vector<double> m(levels.size(), 0.0);
            for (std::size_t l = 1; l < levels.size(); ++l)
                if (std::fabs(levels[l]->mean()) > 2.0 * std::sqrt(levels[l]->estimator_variance()))
                    m[l] = std::fabs(levels[l]->mean());
            return std::max(0.5, -mlmc_detail::log2_slope(m, -1.0));
        }
    } // namespace mlqmc_detail

    /**
     * @brief Giles & Waterhouse's multilevel QMC for a target RMS error.
     *
     * The result's levels carry n = R N_l samples and their sum (sum2 is
     * not meaningful: the samples of a replicate are not independent);
     * estimator_variance holds each level's V_l, std_error sqrt(sum V_l).
     */
    inline MlmcResult simulate_mlqmc(const ISimulatableProduct<Real> &product, const ISimulationModel<Real> &model,
                                     const MlqmcSettings &s)
    {
        if (!(s.eps > 0.0))
            throw InvalidInput("simulate_mlqmc: eps must be > 0");
        if (s.min_finest_level < 1 || s.max_finest_level < s.min_finest_level || s.max_finest_level > 20)
            throw InvalidInput("simulate_mlqmc: 1 <= min_finest_level <= max_finest_level <= 20");
        if (s.replicates < 2)
            throw InvalidInput("simulate_mlqmc: at least 2 replicates");
        if (s.initial_points < 1 || (s.initial_points & (s.initial_points - 1)) != 0)
            throw InvalidInput("simulate_mlqmc: initial_points must be a power of two");

        std::vector<std::unique_ptr<mlqmc_detail::QmcLevel>> levels;
        auto add_level = [&]
        {
            levels.push_back(std::make_unique<mlqmc_detail::QmcLevel>(product, model,
                                                                      static_cast<int>(levels.size()), s));
            levels.back()->add(s.initial_points);
        };
        for (int l = 0; l <= s.min_finest_level; ++l)
            add_level();

        MlmcResult res;
        for (;;)
        {
            // Variance: double the points of the level whose variance per
            // unit of cost falls most, until sum V_l <= eps^2 / 2.
            for (;;)
            {
                double total = 0.0, best = -1.0;
                std::size_t arg = 0;
                for (std::size_t l = 0; l < levels.size(); ++l)
                {
                    const double v = levels[l]->estimator_variance();
                    total += v;
                    const double score = v / (levels[l]->cost() * static_cast<double>(levels[l]->points()));
                    if (score > best)
                        best = score, arg = l;
                }
                if (total <= 0.5 * s.eps * s.eps)
                    break;
                levels[arg]->add(levels[arg]->points());
            }

            // Bias: P4's test on the last two levels.
            const std::size_t L = levels.size() - 1;
            const double alpha = mlqmc_detail::alpha_estimate(levels);
            std::vector<double> m;
            for (const auto &lv : levels)
                m.push_back(std::fabs(lv->mean()));
            res.bias = mlmc_detail::remaining_bias(m, alpha);
            if (res.bias <= s.eps / std::sqrt(2.0))
                break;
            if (static_cast<int>(L) >= s.max_finest_level)
            {
                res.bias_converged = false;
                break;
            }
            add_level();
        }

        res.value = 0.0;
        double var = 0.0;
        std::vector<double> v, c;
        for (const auto &lv : levels)
        {
            MlmcLevel out;
            out.n = lv->points() * s.replicates;
            out.sum = lv->mean() * static_cast<double>(out.n);
            out.cost = lv->cost();
            res.levels.push_back(out);
            res.estimator_variance.push_back(lv->estimator_variance());
            res.value += lv->mean();
            var += lv->estimator_variance();
            // per-sample equivalent variance, for the beta regression
            v.push_back(lv->estimator_variance() * static_cast<double>(out.n));
            c.push_back(lv->cost());
        }
        res.std_error = std::sqrt(var);
        res.alpha = mlqmc_detail::alpha_estimate(levels);
        res.beta = -mlmc_detail::log2_slope(v, -1.0);
        res.gamma = mlmc_detail::log2_slope(c, 1.0);

        std::ostringstream os;
        os << "multilevel QMC (time step, Sobol " << (s.owen ? "Owen-scrambled" : "digital shift") << ", "
           << s.replicates << " replicates), levels 0.." << res.finest_level() << ", points per replicate";
        for (const auto &lv : levels)
            os << ' ' << lv->points();
        os << "; alpha " << res.alpha << ", beta " << res.beta << ", gamma " << res.gamma;
        if (!res.bias_converged)
            os << "; bias test failed at the finest level allowed (" << res.bias << ")";
        res.diagnostics = os.str();
        return res;
    }

} // namespace quantModeling

#endif // QM_ENGINES_MC_MLQMC_HPP
