#ifndef QM_ENGINES_MC_MLQMC_HPP
#define QM_ENGINES_MC_MLQMC_HPP

#include "quantModeling/engines/mc/mlmc.hpp"
#include "quantModeling/utils/brownian_bridge.hpp"
#include "quantModeling/utils/sobol.hpp"
#include "quantModeling/utils/thread_pool.hpp"

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
        /// Target root-mean-square error, absolute; with rel_eps, the floor
        /// of the target (an option worth almost nothing still stops).
        double eps = 0.0;
        /// Target relative to the price: the run aims at max(eps, rel_eps
        /// |estimate|), the estimate updated as levels and points are added
        /// (WP 26 §9.4: the default route's target, 0.2 % of the price).
        double rel_eps = 0.0;
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
            {
                for (int r = 0; r < s.replicates; ++r)
                {
                    auto rep = std::make_unique<Replicate>(product, model, level, s.seed);
                    const std::size_t dim = rep->sampler.draws().size();
                    if (dim > static_cast<std::size_t>(sobol_detail::kMaxDimension))
                        throw InvalidInput("multilevel QMC: level " + std::to_string(level) + " needs " +
                                           std::to_string(dim) + " draws per path, more than the " +
                                           std::to_string(sobol_detail::kMaxDimension) + " Sobol dimensions");
                    // Independent across levels and replicates.
                    const std::uint64_t key = (s.seed << 32) ^ (static_cast<std::uint64_t>(level) << 16) ^
                                              static_cast<std::uint64_t>(r);
                    rep->seq.emplace(static_cast<int>(dim), key, s.owen);
                    const BrownianLayout layout = rep->sampler.fine_layout();
                    if (layout.covers(dim))
                        rep->bridge.emplace(layout.times, layout.factors, layout.stride);
                    rep->point.resize(dim);
                    reps_.push_back(std::move(rep));
                }
                cost_ = reps_.front()->sampler.cost();
                concurrent_ = reps_.front()->sampler.concurrent();
            }

            /// `n` more points on every replicate; the replicates run as tasks
            /// on `pool` when the product allows it. Each replicate's sum
            /// accumulates in point order on its own: the same bits on any
            /// number of threads.
            void add(long long n, ThreadPool *pool)
            {
                run_in_chunks(concurrent_ ? pool : nullptr, reps_.size(), 1,
                              [&](std::size_t first, std::size_t last)
                              {
                                  for (std::size_t r = first; r < last; ++r)
                                      reps_[r]->add(n);
                              });
                points_ += n;
            }

            long long points() const { return points_; }
            double cost() const { return cost_; }

            double mean() const
            {
                double m = 0.0;
                for (const auto &rep : reps_)
                    m += rep->sum;
                return m / (static_cast<double>(reps_.size()) * static_cast<double>(points_));
            }

            /// Variance of the level's estimate: the replicate means' sample
            /// variance over R.
            double estimator_variance() const
            {
                const double m = mean(), R = static_cast<double>(reps_.size());
                double v = 0.0;
                for (const auto &rep : reps_)
                {
                    const double d = rep->sum / static_cast<double>(points_) - m;
                    v += d * d;
                }
                return v / (R - 1.0) / R;
            }

          private:
            struct Replicate
            {
                Replicate(const ISimulatableProduct<Real> &product, const ISimulationModel<Real> &model, int level,
                          std::uint64_t seed)
                    : sampler(product, model, level, seed)
                {
                }
                void add(long long n)
                {
                    for (long long p = 0; p < n; ++p)
                    {
                        seq->next_gaussian(point);
                        if (bridge)
                            bridge->map(point, sampler.draws());
                        else
                            std::copy(point.begin(), point.end(), sampler.draws().begin());
                        sum += sampler.evaluate();
                    }
                }
                mlmc_detail::LevelSampler sampler;
                std::optional<SobolSequence> seq;
                std::optional<BridgedGaussians> bridge;
                std::vector<double> point;
                double sum = 0.0;
            };
            std::vector<std::unique_ptr<Replicate>> reps_;
            long long points_ = 0;
            double cost_ = 0.0;
            bool concurrent_ = false;
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
                                     const MlqmcSettings &s, ThreadPool *pool = nullptr)
    {
        if (!(s.eps > 0.0))
            throw InvalidInput("simulate_mlqmc: eps must be > 0 (with rel_eps, the target's floor)");
        if (s.rel_eps < 0.0)
            throw InvalidInput("simulate_mlqmc: rel_eps must be >= 0");
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
            levels.back()->add(s.initial_points, pool);
        };
        for (int l = 0; l <= s.min_finest_level; ++l)
            add_level();

        // The target: absolute, or relative to the running estimate with
        // eps as its floor.
        auto target = [&]
        {
            double value = 0.0;
            for (const auto &lv : levels)
                value += lv->mean();
            return std::max(s.eps, s.rel_eps * std::fabs(value));
        };

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
                const double eps = target();
                if (total <= 0.5 * eps * eps)
                    break;
                levels[arg]->add(levels[arg]->points(), pool);
            }

            // Bias: P4's test on the last two levels.
            const std::size_t L = levels.size() - 1;
            const double alpha = mlqmc_detail::alpha_estimate(levels);
            std::vector<double> m;
            for (const auto &lv : levels)
                m.push_back(std::fabs(lv->mean()));
            res.bias = mlmc_detail::remaining_bias(m, alpha);
            if (res.bias <= target() / std::sqrt(2.0))
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
        res.eps = target();
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
