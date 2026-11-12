#ifndef QM_ENGINES_MC_MLMC_HPP
#define QM_ENGINES_MC_MLMC_HPP

#include "quantModeling/core/sample.hpp"
#include "quantModeling/core/types.hpp"
#include "quantModeling/instruments/simulatable.hpp"
#include "quantModeling/models/simulation_model.hpp"
#include "quantModeling/utils/rng.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

/**
 * @file mlmc.hpp
 * @brief Multilevel Monte Carlo over the time step, for any product and any
 *        model that discretises in time (blueprint/wp/26-performance.md §8,
 *        WP 26 P4a).
 *
 * Level l simulates the model on its own grid with every step split into
 * 2^l (ISimulationModel::set_refinement), so level l-1's dates are level l's.
 * A sample of level l >= 1 is P_l - P_{l-1}, both payoffs on the same
 * Brownian path: the engine draws the fine increments and sums them over each
 * coarse step for the coarse path (a departure from §8.3, which had the model
 * generate both paths: the model only steps on its grid, the coupling is the
 * engine's, through brownian_layout(), so any model that describes its
 * increments couples without knowing it). Levels draw from independent PCG32
 * streams keyed by (seed, level); a level's later samples continue its
 * stream, so a run is reproducible.
 *
 * The estimator is the payoff of the product's first label. Its cost unit is
 * a model step: a level-l sample costs the fine and the coarse grids' steps.
 */
namespace quantModeling
{

    /// One level's running sums.
    struct MlmcLevel
    {
        long long n = 0;  ///< samples
        double sum = 0.0; ///< of Y_l (P_0 on level 0, P_l - P_{l-1} above)
        double sum2 = 0.0;
        double cost = 0.0; ///< model steps per sample

        double mean() const { return n > 0 ? sum / static_cast<double>(n) : 0.0; }
        double variance() const
        {
            if (n < 2)
                return 0.0;
            const double m = mean();
            return std::max(0.0, (sum2 - static_cast<double>(n) * m * m) / static_cast<double>(n - 1));
        }
    };

    struct MlmcSettings
    {
        double eps = 0.0;         ///< target root-mean-square error, absolute (> 0)
        int min_finest_level = 2; ///< the adaptive run starts with levels 0..min_finest_level
        int max_finest_level = 10;
        long long pilot_paths = 2000; ///< first samples of every level
        std::uint64_t seed = 1;
    };

    struct MlmcResult
    {
        double value = 0.0;
        double std_error = 0.0; ///< sqrt(sum V_l / N_l)
        double bias = 0.0;      ///< the estimate of |E[P - P_L]| the bias test used (adaptive runs)
        double eps = 0.0;       ///< the target RMS error the run stopped at (adaptive runs)
        bool bias_converged = true;
        double alpha = 0.0, beta = 0.0, gamma = 0.0; ///< |E Y_l| ~ 2^-alpha l, V_l ~ 2^-beta l, C_l ~ 2^gamma l
        std::vector<MlmcLevel> levels;
        /// Each level's V_l / N_l, the variance of its estimate (filled by
        /// multilevel QMC, mlqmc.hpp, whose samples are not independent).
        std::vector<double> estimator_variance;
        std::string diagnostics;

        int finest_level() const { return static_cast<int>(levels.size()) - 1; }
        double total_cost() const
        {
            double c = 0.0;
            for (const MlmcLevel &l : levels)
                c += static_cast<double>(l.n) * l.cost;
            return c;
        }
    };

    namespace mlmc_detail
    {
        inline int refinement_of(int level)
        {
            return 1 << level;
        }

        /// A model at one refinement, initialised on the product.
        inline std::unique_ptr<ISimulationModel<Real>> at_refinement(const ISimulationModel<Real> &base, int r,
                                                                     const ISimulatableProduct<Real> &product)
        {
            std::unique_ptr<ISimulationModel<Real>> m = base.clone();
            if (!m->set_refinement(r))
                throw InvalidInput("multilevel Monte Carlo: the model does not refine its time grid (it has no "
                                   "time-step bias to remove, or does not support it yet)");
            m->init(product.timeline(), product.defline());
            return m;
        }

        /**
         * @brief Draws and prices the samples of one level.
         *
         * Level 0 is the model at refinement 1. Level l >= 1 holds two models,
         * at 2^l and 2^(l-1); each coarse Brownian increment is the sum of the
         * fine ones over its step, rescaled to N(0, 1):
         * z_c = sum_k sqrt(dt_k) z_k / sqrt(sum_k dt_k).
         */
        class LevelSampler
        {
          public:
            LevelSampler(const ISimulatableProduct<Real> &product, const ISimulationModel<Real> &base, int level,
                         std::uint64_t seed)
                : product_(product), level_(level), rng_(seed, static_cast<std::uint64_t>(level))
            {
                state_ = product.make_payoff_state();
                fine_ = at_refinement(base, refinement_of(level), product);
                allocate_scenario(fine_path_, product.defline(), product.n_underlyings());
                gauss_fine_.resize(fine_->sim_dim());
                cost_ = static_cast<double>(fine_->sim_dim());
                if (level == 0)
                    return;

                coarse_ = at_refinement(base, refinement_of(level - 1), product);
                allocate_scenario(coarse_path_, product.defline(), product.n_underlyings());
                gauss_coarse_.resize(coarse_->sim_dim());
                cost_ += static_cast<double>(coarse_->sim_dim());

                const BrownianLayout lf = fine_->brownian_layout(), lc = coarse_->brownian_layout();
                if (!lf.covers(fine_->sim_dim()) || !lc.covers(coarse_->sim_dim()) || lf.stride != lf.factors ||
                    lc.stride != lc.factors || lf.factors != lc.factors)
                    throw InvalidInput("multilevel Monte Carlo: the model's draws are not all Brownian increments "
                                       "(jumps?), the levels cannot be coupled");
                factors_ = lf.factors;
                // Each fine step's coarse step, and the sqrt(dt) weights. The
                // coarse dates are fine dates exactly (refine_steps), hence
                // the tight tolerance: TIMELINE_EPS (an hour) is longer than
                // a fine level's step.
                coarse_of_.resize(lf.times.size());
                weight_.resize(lf.times.size());
                std::vector<double> coarse_dt(lc.times.size(), 0.0);
                std::size_t c = 0;
                Time prev = 0.0;
                for (std::size_t k = 0; k < lf.times.size(); ++k)
                {
                    const Time t = lf.times[k];
                    while (c < lc.times.size() && lc.times[c] < t - 1e-12 * std::max(1.0, t))
                        ++c;
                    if (c == lc.times.size())
                        throw InvalidInput("multilevel Monte Carlo: the fine grid goes past the coarse one");
                    coarse_of_[k] = c;
                    weight_[k] = std::sqrt(t - prev);
                    coarse_dt[c] += t - prev;
                    prev = t;
                }
                for (std::size_t k = 0; k < weight_.size(); ++k)
                    weight_[k] /= std::sqrt(coarse_dt[coarse_of_[k]]);
            }

            double cost() const { return cost_; }

            void run(long long n, MlmcLevel &acc)
            {
                acc.cost = cost_;
                for (long long p = 0; p < n; ++p)
                {
                    for (double &z : gauss_fine_)
                        z = bm_(rng_);
                    const double y = evaluate();
                    ++acc.n;
                    acc.sum += y;
                    acc.sum2 += y * y;
                }
            }

            /// The fine level's draws, time order (BrownianLayout): fill
            /// them, then evaluate() (P5's Sobol points go through here).
            std::vector<double> &draws() { return gauss_fine_; }
            BrownianLayout fine_layout() const { return fine_->brownian_layout(); }

            /// The sample of the draws in draws(): P_l, minus P_{l-1} on the
            /// summed increments above level 0.
            double evaluate()
            {
                fine_->generate_path(gauss_fine_, fine_path_);
                payoffs(fine_path_);
                double y = out_.at(0);
                if (level_ > 0)
                {
                    std::fill(gauss_coarse_.begin(), gauss_coarse_.end(), 0.0);
                    for (std::size_t k = 0; k < coarse_of_.size(); ++k)
                        for (std::size_t f = 0; f < factors_; ++f)
                            gauss_coarse_[coarse_of_[k] * factors_ + f] += weight_[k] * gauss_fine_[k * factors_ + f];
                    coarse_->generate_path(gauss_coarse_, coarse_path_);
                    payoffs(coarse_path_);
                    y -= out_.at(0);
                }
                return y;
            }

            /// Whether several samplers of this product may run at once
            /// (the product gives each its own payoff state).
            bool concurrent() const { return state_ != nullptr; }

          private:
            void payoffs(const Scenario<Real> &path)
            {
                if (state_)
                    product_.payoffs_with_state(path, out_, *state_);
                else
                    product_.payoffs(path, out_);
            }

            const ISimulatableProduct<Real> &product_;
            std::unique_ptr<typename ISimulatableProduct<Real>::PayoffState> state_;
            int level_;
            Pcg32 rng_;
            NormalBoxMuller bm_;
            std::unique_ptr<ISimulationModel<Real>> fine_, coarse_;
            Scenario<Real> fine_path_, coarse_path_;
            std::vector<double> gauss_fine_, gauss_coarse_;
            std::vector<Real> out_;
            std::size_t factors_ = 0;
            std::vector<std::size_t> coarse_of_;
            std::vector<double> weight_;
            double cost_ = 0.0;
        };

        /// Least-squares slope of log2 y against l over the levels from 1 with
        /// y > 0; `fallback` when fewer than two such levels.
        inline double log2_slope(const std::vector<double> &y, double fallback)
        {
            double n = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
            for (std::size_t l = 1; l < y.size(); ++l)
                if (y[l] > 0.0)
                {
                    const double x = static_cast<double>(l), v = std::log2(y[l]);
                    n += 1;
                    sx += x;
                    sy += v;
                    sxx += x * x;
                    sxy += x * v;
                }
            if (n < 2)
                return fallback;
            return (n * sxy - sx * sy) / (n * sxx - sx * sx);
        }

        /**
         * alpha by regression over the levels from 1 whose sample mean is
         * resolved, |mean| > 2 standard errors (WP 26 P4b). A mean lost in
         * its noise says nothing of the decay, and regressing on it swung
         * the estimate from 0.66 to 2.2 between runs (P4a), which the bias
         * test divides by. Fewer than two resolved levels: 1, the weak order
         * of both Euler and Milstein. At least 0.5, as Giles.
         */
        inline double alpha_estimate(const std::vector<MlmcLevel> &levels)
        {
            std::vector<double> m(levels.size(), 0.0);
            for (std::size_t l = 1; l < levels.size(); ++l)
            {
                const MlmcLevel &v = levels[l];
                if (v.n < 2)
                    continue;
                const double se = std::sqrt(v.variance() / static_cast<double>(v.n));
                if (std::fabs(v.mean()) > 2.0 * se)
                    m[l] = std::fabs(v.mean());
            }
            return std::max(0.5, -log2_slope(m, -1.0));
        }

        /**
         * The remaining bias after level L, from the last three correction
         * levels (Giles's later code, range -2:0): max over l in {L-2, L-1,
         * L}, l >= 1, of |Y_l| / r^(L - l), over r - 1, r = 2^alpha. Two
         * levels (Giles 2008) assume the decay is already geometric; on
         * SPY's local-vol grid the corrections fell fast then slowly (1.06,
         * 0.12, 0.03, 0.06, 0.06), and the two-level test stopped at level 2
         * with a bias about twice its target (WP 26 §9.2). Level 0 is the
         * price itself, never a correction.
         */
        inline double remaining_bias(const std::vector<double> &m, double alpha)
        {
            const double r = std::pow(2.0, alpha);
            const std::size_t L = m.size() - 1;
            double worst = 0.0;
            for (std::size_t back = 0; back <= 2 && back < L; ++back)
                worst = std::max(worst, m[L - back] / std::pow(r, static_cast<double>(back)));
            return worst / (r - 1.0);
        }

        inline void finish(MlmcResult &res)
        {
            res.value = 0.0;
            double var = 0.0;
            for (const MlmcLevel &l : res.levels)
            {
                res.value += l.mean();
                if (l.n > 0)
                    var += l.variance() / static_cast<double>(l.n);
            }
            res.std_error = std::sqrt(var);
            std::vector<double> m, v, c;
            for (const MlmcLevel &l : res.levels)
            {
                m.push_back(std::fabs(l.mean()));
                v.push_back(l.variance());
                c.push_back(l.cost);
            }
            res.alpha = alpha_estimate(res.levels);
            res.beta = -log2_slope(v, -1.0);
            res.gamma = log2_slope(c, 1.0);

            std::ostringstream os;
            os << "multilevel Monte Carlo (time step), levels 0.." << res.finest_level() << ", paths";
            for (const MlmcLevel &l : res.levels)
                os << ' ' << l.n;
            os << "; alpha " << res.alpha << ", beta " << res.beta << ", gamma " << res.gamma;
            if (!res.bias_converged)
                os << "; bias test failed at the finest level allowed (" << res.bias << ")";
            res.diagnostics = os.str();
        }
    } // namespace mlmc_detail

    /**
     * @brief Levels 0..n.size()-1 with n[l] samples each: an unbiased
     *        estimate of the finest level's E[P_L] (the telescoping sum),
     *        for tests and measurements.
     */
    inline MlmcResult simulate_mlmc_fixed(const ISimulatableProduct<Real> &product, const ISimulationModel<Real> &model,
                                          const std::vector<long long> &n, std::uint64_t seed)
    {
        if (n.empty())
            throw InvalidInput("simulate_mlmc_fixed: at least one level");
        MlmcResult res;
        res.levels.resize(n.size());
        for (std::size_t l = 0; l < n.size(); ++l)
        {
            mlmc_detail::LevelSampler sampler(product, model, static_cast<int>(l), seed);
            sampler.run(n[l], res.levels[l]);
        }
        mlmc_detail::finish(res);
        return res;
    }

    /**
     * @brief Giles's adaptive algorithm (Giles 2008; Giles 2015, Acta
     *        Numerica §3), M = 2: a pilot of every level, then
     *        N_l = ceil(2 eps^-2 sqrt(V_l / C_l) sum_k sqrt(V_k C_k)) (half the
     *        mean-square budget to the variance), and a level added while
     *        the remaining bias of the last three corrections
     *        (mlmc_detail::remaining_bias) exceeds eps / sqrt(2)
     *        (the other half to the bias). alpha and beta are estimated by
     *        regression over the levels (alpha over the resolved ones only,
     *        see mlmc_detail::alpha_estimate), never assumed;
     *        a level whose sample mean or variance is below what the decay
     *        predicts from the previous one is lifted to half that
     *        prediction (Giles's guard against a lucky small estimate).
     */
    inline MlmcResult simulate_mlmc(const ISimulatableProduct<Real> &product, const ISimulationModel<Real> &model,
                                    const MlmcSettings &s)
    {
        if (!(s.eps > 0.0))
            throw InvalidInput("simulate_mlmc: eps must be > 0");
        if (s.min_finest_level < 1 || s.max_finest_level < s.min_finest_level || s.max_finest_level > 20)
            throw InvalidInput("simulate_mlmc: 1 <= min_finest_level <= max_finest_level <= 20");
        if (s.pilot_paths < 2)
            throw InvalidInput("simulate_mlmc: pilot_paths must be >= 2");

        MlmcResult res;
        std::vector<std::unique_ptr<mlmc_detail::LevelSampler>> samplers;
        std::vector<long long> dn;
        auto add_level = [&]
        {
            const int l = static_cast<int>(samplers.size());
            samplers.push_back(std::make_unique<mlmc_detail::LevelSampler>(product, model, l, s.seed));
            res.levels.emplace_back();
            res.levels.back().cost = samplers.back()->cost();
            dn.push_back(0);
        };
        for (int l = 0; l <= s.min_finest_level; ++l)
        {
            add_level();
            dn[static_cast<std::size_t>(l)] = s.pilot_paths;
        }

        for (;;)
        {
            for (std::size_t l = 0; l < samplers.size(); ++l)
                if (dn[l] > 0)
                    samplers[l]->run(dn[l], res.levels[l]);

            const std::size_t L = res.levels.size() - 1;
            std::vector<double> m(L + 1), v(L + 1), c(L + 1);
            for (std::size_t l = 0; l <= L; ++l)
            {
                m[l] = std::fabs(res.levels[l].mean());
                v[l] = res.levels[l].variance();
                c[l] = res.levels[l].cost;
            }
            const double alpha = mlmc_detail::alpha_estimate(res.levels);
            const double beta = std::max(0.5, -mlmc_detail::log2_slope(v, -1.0));
            for (std::size_t l = 2; l <= L; ++l)
            {
                m[l] = std::max(m[l], 0.5 * m[l - 1] / std::pow(2.0, alpha));
                v[l] = std::max(v[l], 0.5 * v[l - 1] / std::pow(2.0, beta));
            }

            auto targets = [&]
            {
                double sum = 0.0;
                for (std::size_t l = 0; l < v.size(); ++l)
                    sum += std::sqrt(v[l] * c[l]);
                for (std::size_t l = 0; l < v.size(); ++l)
                {
                    const double ns = std::ceil(2.0 / (s.eps * s.eps) * std::sqrt(v[l] / c[l]) * sum);
                    dn[l] = std::max(0LL, static_cast<long long>(ns) - res.levels[l].n);
                }
            };
            targets();

            bool sampled = true;
            for (std::size_t l = 0; l <= L; ++l)
                if (static_cast<double>(dn[l]) > 0.01 * static_cast<double>(res.levels[l].n))
                    sampled = false;
            if (!sampled)
                continue;

            const double r = std::pow(2.0, alpha);
            res.bias = mlmc_detail::remaining_bias(m, alpha);
            if (res.bias <= s.eps / std::sqrt(2.0))
                break;
            if (static_cast<int>(L) >= s.max_finest_level)
            {
                res.bias_converged = false;
                break;
            }
            add_level();
            m.push_back(m[L] / r);
            v.push_back(v[L] / std::pow(2.0, beta));
            c.push_back(res.levels.back().cost);
            targets();
        }

        res.eps = s.eps;
        mlmc_detail::finish(res);
        return res;
    }

} // namespace quantModeling

#endif // QM_ENGINES_MC_MLMC_HPP
