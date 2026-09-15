#ifndef QM_MODELS_EQUITY_MERTON_JUMP_DIFFUSION_SIM_MODEL_HPP
#define QM_MODELS_EQUITY_MERTON_JUMP_DIFFUSION_SIM_MODEL_HPP

#include "quantModeling/aad/number.hpp"
#include "quantModeling/core/types.hpp"
#include "quantModeling/models/simulation_model.hpp"
#include "quantModeling/utils/stats.hpp"

#include <cmath>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace quantModeling
{

    /**
     * @brief Merton (1976) jump-diffusion for the timeline simulation
     *        architecture — the project's first model with discontinuous
     *        paths.
     *
     *   dS/S = (r - q - lambda*k) dt + sigma dW + dJ
     *
     * J is a compound Poisson process: jumps arrive at intensity lambda, and
     * each jump's *log*-size is ~ N(jump_mean, jump_vol^2), independent and
     * identically distributed. k = E[Y-1] = exp(jump_mean + jump_vol^2/2) - 1
     * is the compensator that keeps the discounted stock a martingale under
     * the risk-neutral measure -- without it, the jumps alone would drift the
     * price.
     *
     * Exact simulation, no Euler discretisation: conditional on N jumps
     * during a step, the sum of N i.i.d. N(jump_mean, jump_vol^2) log-jumps
     * is itself exactly N(N*jump_mean, N*jump_vol^2), so one big step per
     * event date is exact, the same way Black-Scholes's is -- just with two
     * extra draws to sample N and that conditional sum.
     *
     * lambda, jump_mean and jump_vol are T: with T = aad::Number every jump
     * parameter is a differentiable model parameter (blueprint §6.2), on
     * exactly the same footing as spot/rate/div/vol. One honest limit,
     * consistent with how a comparison or a local-vol grid cell already work
     * in this architecture: *how many* jumps occur on a path is decided from
     * a plain double draw (control flow is not differentiated), only the
     * *size* of the jumps that do occur, and the deterministic compensator
     * every path uses regardless of its own jump count, carry a gradient.
     */
    template <class T = Real>
    class MertonJumpDiffusionSimModel final : public ISimulationModel<T>
    {
      public:
        MertonJumpDiffusionSimModel(T s0, T r, T q, T sigma, T lambda,
                                    T jump_mean, T jump_vol)
            : s0_(s0), r_(r), q_(q), sigma_(sigma), lambda_(lambda),
              jump_mean_(jump_mean), jump_vol_(jump_vol)
        {
            if (to_double(lambda_) < 0.0)
                throw InvalidInput("MertonJumpDiffusionSimModel: lambda must be >= 0");
            if (to_double(jump_vol_) < 0.0)
                throw InvalidInput("MertonJumpDiffusionSimModel: jump_vol must be >= 0");
            set_param_pointers();
        }

        /// See models/equity/bs_sim_model.hpp for why this exists: params_
        /// caches pointers into *this* object's own members, so a naive
        /// copy would leave them pointing at the original.
        MertonJumpDiffusionSimModel(const MertonJumpDiffusionSimModel &other)
            : s0_(other.s0_), r_(other.r_), q_(other.q_), sigma_(other.sigma_),
              lambda_(other.lambda_), jump_mean_(other.jump_mean_),
              jump_vol_(other.jump_vol_), timeline_(other.timeline_),
              defline_(other.defline_), steps_(other.steps_),
              sim_dim_(other.sim_dim_)
        {
            set_param_pointers();
        }

        std::size_t n_underlyings() const override { return 1; }

        void init(const TimeLine &product_timeline,
                  const std::vector<SampleDef> &defline) override
        {
            using std::exp;

            timeline_ = canonical_timeline(product_timeline);
            defline_ = defline;

            // k = E[Y - 1], the compensator -- computed once, T-typed: it is
            // a deterministic function of jump_mean/jump_vol, used by every
            // path regardless of how many jumps that path itself draws.
            const T k = exp(jump_mean_ + 0.5 * jump_vol_ * jump_vol_) - 1.0;
            const T mu = r_ - q_ - 0.5 * sigma_ * sigma_ - lambda_ * k;

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
                    using std::sqrt;
                    const Time dt = t - t_prev;
                    s.dt = dt;
                    s.drift = mu * dt;
                    s.vol_sqrt_dt = sigma_ * sqrt(dt);
                    sim_dim_ += 3; // diffusion, jump-count, jump-sum -- see generate_path()
                }
                steps_.push_back(s);
                t_prev = t;
            }
        }

        const TimeLine &sim_timeline() const override { return timeline_; }
        std::size_t sim_dim() const override { return sim_dim_; }

        void generate_path(std::span<const double> gaussians,
                           Scenario<T> &path) const override
        {
            using std::exp;

            T S = s0_;
            std::size_t g = 0;
            for (std::size_t i = 0; i < steps_.size(); ++i)
            {
                const Step &st = steps_[i];
                if (st.draws)
                {
                    const double z_diffusion = gaussians[g++];
                    // A uniform via the probability integral transform: the
                    // engine only ever hands out gaussians, never a raw
                    // uniform, so Phi(Z) for Z ~ N(0,1) is how one is
                    // recovered here.
                    const double u = norm_cdf(gaussians[g++]);
                    const double z_jump_sum = gaussians[g++];

                    const int n_jumps = sample_poisson(u, to_double(lambda_) * st.dt);
                    T total = S * exp(st.drift + st.vol_sqrt_dt * z_diffusion);
                    if (n_jumps > 0)
                    {
                        const double n = static_cast<double>(n_jumps);
                        using std::sqrt;
                        const T jump_sum =
                            n * jump_mean_ + sqrt(n) * jump_vol_ * z_jump_sum;
                        total = total * exp(jump_sum);
                    }
                    S = total;
                }

                Sample<T> &smp = path[i];
                smp.spots.assign(1, S);
                smp.numeraire = exp(r_ * st.t);

                const SampleDef &def = defline_[i];
                smp.discounts.resize(def.discount_mats.size());
                for (std::size_t m = 0; m < def.discount_mats.size(); ++m)
                    smp.discounts[m] = exp(-r_ * (def.discount_mats[m] - st.t));

                smp.forwards.resize(def.forward_mats.size());
                for (std::size_t m = 0; m < def.forward_mats.size(); ++m)
                {
                    const Time mat = def.forward_mats[m];
                    smp.forwards[m] = S * exp((r_ - q_) * (mat - st.t));
                }
            }
        }

        std::unique_ptr<ISimulationModel<T>> clone() const override
        {
            return std::make_unique<MertonJumpDiffusionSimModel<T>>(*this);
        }

        const std::vector<T *> &parameters() const override { return params_; }
        const std::vector<std::string> &parameter_labels() const override
        {
            static const std::vector<std::string> labels{
                "spot", "rate", "div", "vol", "jump_intensity", "jump_mean", "jump_vol"};
            return labels;
        }

      private:
        struct Step
        {
            Time t = 0.0;
            Time dt = 0.0;
            bool draws = false;
            T drift{};
            T vol_sqrt_dt{};
        };

        /// Inverse-transform sampling of a Poisson(mean) count from a
        /// uniform u in (0,1). Terminates almost immediately for the small
        /// means realistic jump parameters produce (lambda*dt << 1 for a
        /// discretely-monitored product); max_n is a generous, purely
        /// defensive ceiling against a pathological mean.
        static int sample_poisson(double u, double mean)
        {
            if (mean <= 0.0)
                return 0;
            constexpr int max_n = 200;
            double p = std::exp(-mean);
            double cumulative = p;
            int n = 0;
            while (u > cumulative && n < max_n)
            {
                ++n;
                p *= mean / static_cast<double>(n);
                cumulative += p;
            }
            return n;
        }

        void set_param_pointers()
        {
            params_ = {&s0_, &r_, &q_, &sigma_, &lambda_, &jump_mean_, &jump_vol_};
        }

        T s0_{}, r_{}, q_{}, sigma_{}, lambda_{}, jump_mean_{}, jump_vol_{};
        TimeLine timeline_;
        std::vector<SampleDef> defline_;
        std::vector<Step> steps_;
        std::size_t sim_dim_ = 0;
        std::vector<T *> params_;
    };

} // namespace quantModeling

#endif // QM_MODELS_EQUITY_MERTON_JUMP_DIFFUSION_SIM_MODEL_HPP
