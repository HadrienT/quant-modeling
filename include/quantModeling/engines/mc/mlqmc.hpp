#ifndef QM_ENGINES_MC_MLQMC_HPP
#define QM_ENGINES_MC_MLQMC_HPP

#include "quantModeling/engines/mc/mlmc.hpp"
#include "quantModeling/utils/brownian_bridge.hpp"
#include "quantModeling/utils/sobol.hpp"
#include "quantModeling/utils/thread_pool.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
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
 * eps^2 / 2; the bias test and the levels added are P4's, the test decided
 * on an estimate resolved against its own error, the levels bounded by what
 * the device runs and by a deadline (issue #383, WP 26 §28).
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
        bool owen = false;               ///< Owen's hash-based scrambling instead of the digital shift
        bool coarse_conditioning = true; ///< as MlmcSettings::coarse_conditioning (WP 26 §26)
        MlmcDeadline deadline;           ///< as MlmcSettings::deadline
    };

    /// One level of a multilevel QMC run, wherever its points are priced:
    /// simulate_mlqmc_levels() only reads these (the CPU's levels below, the
    /// GPU's in src/engines/mc/script_engine.cpp, WP 26 P6).
    class IQmcLevel
    {
      public:
        virtual ~IQmcLevel() = default;
        /// `n` more points on every replicate.
        virtual void add(long long n) = 0;
        virtual long long points() const = 0; ///< per replicate
        virtual double cost() const = 0;      ///< of one point, in model draws
        virtual double mean() const = 0;      ///< the level's estimate
        /// Variance of the level's estimate: the replicate means' sample
        /// variance over R.
        virtual double estimator_variance() const = 0;
    };

    namespace mlqmc_detail
    {
        /// Mean and variance of a level's estimate from its R replicate
        /// means.
        inline double replicate_mean(const std::vector<double> &means)
        {
            double m = 0.0;
            for (const double x : means)
                m += x;
            return m / static_cast<double>(means.size());
        }
        inline double replicate_variance(const std::vector<double> &means)
        {
            const double m = replicate_mean(means), R = static_cast<double>(means.size());
            double v = 0.0;
            for (const double x : means)
                v += (x - m) * (x - m);
            return v / (R - 1.0) / R;
        }

        /// One level's R Sobol replicates, their running sums, and its
        /// sampler.
        class QmcLevel final : public IQmcLevel
        {
          public:
            QmcLevel(const ISimulatableProduct<Real> &product, const ISimulationModel<Real> &model, int level,
                     const MlqmcSettings &s, ThreadPool *pool)
                : pool_(pool)
            {
                for (int r = 0; r < s.replicates; ++r)
                {
                    auto rep = std::make_unique<Replicate>(product, model, level, s.seed, s.coarse_conditioning);
                    const std::size_t dim = rep->sampler.draws().size();
                    if (dim > static_cast<std::size_t>(sobol_detail::kMaxDimension))
                        throw LevelBeyondCapacity("multilevel QMC: level " + std::to_string(level) + " needs " +
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
            void add(long long n) override
            {
                run_in_chunks(concurrent_ ? pool_ : nullptr, reps_.size(), 1,
                              [&](std::size_t first, std::size_t last)
                              {
                                  for (std::size_t r = first; r < last; ++r)
                                      reps_[r]->add(n);
                              });
                points_ += n;
            }

            long long points() const override { return points_; }
            double cost() const override { return cost_; }

            double mean() const override
            {
                double m = 0.0;
                for (const auto &rep : reps_)
                    m += rep->sum;
                return m / (static_cast<double>(reps_.size()) * static_cast<double>(points_));
            }

            /// Variance of the level's estimate: the replicate means' sample
            /// variance over R.
            double estimator_variance() const override
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
                          std::uint64_t seed, bool coarse_conditioning)
                    : sampler(product, model, level, seed, coarse_conditioning)
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
            ThreadPool *pool_ = nullptr;
            long long points_ = 0;
            double cost_ = 0.0;
            bool concurrent_ = false;
        };

        /// alpha over the levels from 1 whose mean is resolved (|mean| > 2
        /// standard errors of the level's estimate), in [0.5, 1], as
        /// mlmc_detail.
        inline double alpha_estimate(const std::vector<std::unique_ptr<IQmcLevel>> &levels)
        {
            std::vector<double> m(levels.size(), 0.0), se(levels.size(), 0.0);
            for (std::size_t l = 1; l < levels.size(); ++l)
            {
                m[l] = levels[l]->mean();
                se[l] = std::sqrt(levels[l]->estimator_variance());
            }
            return std::clamp(mlmc_detail::weighted_alpha(m, se, 1.0), 0.5, 1.0);
        }

        /// The bias test's estimate is resolved when its standard error is
        /// at most this share of the test's threshold (issue #383).
        constexpr double kBiasResolution = 0.25;

        struct BiasEstimate
        {
            double bias = 0.0;
            double std_error = 0.0;
            std::size_t level = 0; ///< the level the estimate comes from
        };

        /// mlmc_detail::remaining_bias, with the standard error of the
        /// level it comes from scaled alike.
        inline BiasEstimate remaining_bias(const std::vector<std::unique_ptr<IQmcLevel>> &levels, double alpha)
        {
            const double r = std::pow(2.0, alpha);
            const std::size_t L = levels.size() - 1;
            BiasEstimate out;
            double worst = -1.0, scale = 1.0;
            for (std::size_t back = 0; back <= 2 && back < L; ++back)
            {
                const double k = 1.0 / std::pow(r, static_cast<double>(back));
                const double x = std::fabs(levels[L - back]->mean()) * k;
                if (x > worst)
                    worst = x, scale = k, out.level = L - back;
            }
            out.bias = worst / (r - 1.0);
            out.std_error = std::sqrt(levels[out.level]->estimator_variance()) * scale / (r - 1.0);
            return out;
        }
    } // namespace mlqmc_detail

    /// Builds level l of a run (its first points are added by the run).
    using QmcLevelFactory = std::function<std::unique_ptr<IQmcLevel>(int)>;

    /**
     * @brief Giles & Waterhouse's multilevel QMC for a target RMS error, on
     *        the levels `make_level` builds.
     *
     * The result's levels carry n = R N_l samples and their sum (sum2 is
     * not meaningful: the samples of a replicate are not independent);
     * estimator_variance holds each level's V_l, std_error sqrt(sum V_l).
     */
    inline MlmcResult simulate_mlqmc_levels(const QmcLevelFactory &make_level, const MlqmcSettings &s)
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

        std::vector<std::unique_ptr<IQmcLevel>> levels;
        auto add_level = [&]
        {
            levels.push_back(make_level(static_cast<int>(levels.size())));
            levels.back()->add(s.initial_points);
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
        auto stop = [&](MlmcStop kind, std::string why)
        {
            res.bias_converged = false;
            res.stop = kind;
            res.stop_reason = std::move(why);
        };
        mlqmc_detail::BiasEstimate bias;
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
                if (total <= 0.5 * eps * eps || past(s.deadline))
                    break;
                levels[arg]->add(levels[arg]->points());
            }

            // Bias: P4's test over the last levels, decided on a resolved
            // estimate (issue #383). The variance step leaves each level's
            // error up to eps / sqrt(2), the test's own threshold: one noisy
            // Y_L stopped SPY's one-touch at level 2 by luck on one surface
            // and escalated it to the Sobol cap on the other. While the
            // estimate is within two of its standard errors of the threshold,
            // the level it comes from doubles its points, down to a quarter
            // of the threshold.
            for (;;)
            {
                bias = mlqmc_detail::remaining_bias(levels, mlqmc_detail::alpha_estimate(levels));
                const double thr = target() / std::sqrt(2.0);
                if (bias.std_error <= mlqmc_detail::kBiasResolution * thr ||
                    std::fabs(bias.bias - thr) >= 2.0 * bias.std_error || past(s.deadline))
                    break;
                levels[bias.level]->add(levels[bias.level]->points());
            }
            if (bias.bias <= target() / std::sqrt(2.0))
                break;
            if (past(s.deadline))
            {
                stop(MlmcStop::TimeLimit, "stopped at its time limit");
                break;
            }
            const int L = static_cast<int>(levels.size()) - 1;
            if (L >= s.max_finest_level)
            {
                stop(MlmcStop::FinestAllowed, "at the finest level allowed (" + std::to_string(L) + ")");
                break;
            }
            // A level the device cannot run ends the run at the finest one
            // it can (issue #383): falling back to another device restarted
            // from level 0 and refined on the CPU long after the route's
            // timeout.
            try
            {
                add_level();
            }
            catch (const LevelBeyondCapacity &e)
            {
                stop(MlmcStop::DeviceCapacity, "at the finest level the device runs (" + std::to_string(L) + "): " + e.what());
                break;
            }
        }
        res.bias = bias.bias;
        res.bias_std_error = bias.std_error;

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
            os << "; bias test failed (" << res.bias << " +- " << res.bias_std_error << "): " << res.stop_reason;
        res.diagnostics = os.str();
        return res;
    }

    /// The same on the CPU: each level's replicates as tasks on `pool`.
    inline MlmcResult simulate_mlqmc(const ISimulatableProduct<Real> &product, const ISimulationModel<Real> &model,
                                     const MlqmcSettings &s, ThreadPool *pool = nullptr)
    {
        return simulate_mlqmc_levels(
            [&](int level) -> std::unique_ptr<IQmcLevel>
            { return std::make_unique<mlqmc_detail::QmcLevel>(product, model, level, s, pool); },
            s);
    }

} // namespace quantModeling

#endif // QM_ENGINES_MC_MLQMC_HPP
