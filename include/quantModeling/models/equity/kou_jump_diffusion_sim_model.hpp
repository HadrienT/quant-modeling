#ifndef QM_MODELS_EQUITY_KOU_JUMP_DIFFUSION_SIM_MODEL_HPP
#define QM_MODELS_EQUITY_KOU_JUMP_DIFFUSION_SIM_MODEL_HPP

#include "quantModeling/aad/number.hpp"
#include "quantModeling/core/types.hpp"
#include "quantModeling/models/simulation_model.hpp"
#include "quantModeling/utils/stats.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace quantModeling
{

    /**
     * @brief Kou (2002) double-exponential jump-diffusion for the timeline
     *        simulation architecture.
     *
     * Same dS/S = (r - q - lambda*k) dt + sigma dW + dJ as
     * MertonJumpDiffusionSimModel, with one difference that changes
     * everything about how it must be simulated: a jump's log-size is not
     * Gaussian but *asymmetric double-exponential* --
     *   f(x) = p*eta1*exp(-eta1*x)      for x >= 0  (up-jumps, mean 1/eta1)
     *          (1-p)*eta2*exp(eta2*x)   for x <  0  (down-jumps, mean -1/eta2)
     * -- which lets it fit the empirical asymmetry between up- and down-move
     * jump risk that a symmetric Merton jump cannot. eta1 > 1 is required
     * for E[Y] to be finite (needed for the compensator below); eta2 > 0.
     *
     * The price of that asymmetry: a sum of N i.i.d. double-exponentials has
     * no closed form the way a sum of N i.i.d. Gaussians does (Merton's
     * exact trick), so this cannot do one exact step per event date the way
     * Merton or Black-Scholes can. Instead, every step draws a *fixed*
     * budget of `max_jumps_per_step` individual jump sizes (each its own
     * inverse-CDF draw) and sums only the first `min(N, max_jumps_per_step)`
     * of them -- exact as long as N never exceeds the budget, which for any
     * realistic lambda*dt (comfortably below 1) it does not: the truncated
     * tail probability is negligible to floating-point precision. This is
     * still not an Euler discretisation of an SDE (the diffusion part is
     * still simulated exactly); the only approximation is that generous,
     * essentially-certain truncation.
     *
     * lambda, p, eta1, eta2 are T: with T = aad::Number every jump
     * parameter is differentiable, on the same footing as spot/rate/div/vol
     * -- including a genuine sensitivity to the up/down jump asymmetry
     * itself. As in MertonJumpDiffusionSimModel, *how many* jumps occur, and
     * *which side* (up or down) an individual jump lands on, are decided
     * from plain double draws (control flow is not differentiated); only
     * the *size* of the jumps that do occur, and the deterministic
     * compensator every path uses, carry a gradient.
     */
    template <class T = Real>
    class KouJumpDiffusionSimModel final : public ISimulationModel<T>
    {
      public:
        KouJumpDiffusionSimModel(T s0, T r, T q, T sigma, T lambda, T p,
                                 T eta1, T eta2, int max_jumps_per_step = 10)
            : s0_(s0), r_(r), q_(q), sigma_(sigma), lambda_(lambda), p_(p), eta1_(eta1), eta2_(eta2), max_jumps_per_step_(max_jumps_per_step)
        {
            if (to_double(lambda_) < 0.0)
                throw InvalidInput("KouJumpDiffusionSimModel: lambda must be >= 0");
            if (to_double(p_) < 0.0 || to_double(p_) > 1.0)
                throw InvalidInput("KouJumpDiffusionSimModel: p must be in [0, 1]");
            if (to_double(eta1_) <= 1.0)
                throw InvalidInput(
                    "KouJumpDiffusionSimModel: eta1 must be > 1 (E[Y] would "
                    "not be finite otherwise)");
            if (to_double(eta2_) <= 0.0)
                throw InvalidInput("KouJumpDiffusionSimModel: eta2 must be > 0");
            if (max_jumps_per_step_ < 1)
                throw InvalidInput(
                    "KouJumpDiffusionSimModel: max_jumps_per_step must be >= 1");
            set_param_pointers();
        }

        /// See models/equity/bs_sim_model.hpp for why this exists.
        KouJumpDiffusionSimModel(const KouJumpDiffusionSimModel &other)
            : s0_(other.s0_), r_(other.r_), q_(other.q_), sigma_(other.sigma_), lambda_(other.lambda_), p_(other.p_), eta1_(other.eta1_), eta2_(other.eta2_), max_jumps_per_step_(other.max_jumps_per_step_), timeline_(other.timeline_), defline_(other.defline_), steps_(other.steps_), sim_dim_(other.sim_dim_)
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

            // k = E[Y - 1] = p*eta1/(eta1-1) + (1-p)*eta2/(eta2+1) - 1
            // (Kou 2002, eq. 4) -- deterministic, T-typed, used by every
            // path regardless of how many jumps that path itself draws.
            const T k = p_ * eta1_ / (eta1_ - 1.0) +
                        (1.0 - p_) * eta2_ / (eta2_ + 1.0) - 1.0;
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
                    // diffusion + poisson-count uniform + one draw per jump slot
                    sim_dim_ += 2 + static_cast<std::size_t>(max_jumps_per_step_);
                }
                steps_.push_back(s);
                t_prev = t;
            }
        }

        const TimeLine &sim_timeline() const override { return timeline_; }
        std::size_t sim_dim() const override { return sim_dim_; }

        BrownianLayout brownian_layout() const override
        {
            BrownianLayout l;
            l.factors = 1;
            l.stride = 2 + static_cast<std::size_t>(max_jumps_per_step_);
            for (const Step &st : steps_)
                if (st.draws)
                    l.times.push_back(st.t);
            return l;
        }

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
                    const double u_poisson = norm_cdf(gaussians[g++]);
                    const int n_jumps = sample_poisson(u_poisson, to_double(lambda_) * st.dt);
                    const int applied = std::min(n_jumps, max_jumps_per_step_);

                    T jump_total{};
                    bool any_jump = false;
                    for (int j = 0; j < max_jumps_per_step_; ++j)
                    {
                        const double u = norm_cdf(gaussians[g++]); // always consumed
                        if (j >= applied)
                            continue; // slot unused this path: no Number built, no tape node
                        const T jump = sample_jump(u);
                        jump_total = any_jump ? jump_total + jump : jump;
                        any_jump = true;
                    }

                    T total = S * exp(st.drift + st.vol_sqrt_dt * z_diffusion);
                    if (any_jump)
                        total = total * exp(jump_total);
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
            return std::make_unique<KouJumpDiffusionSimModel<T>>(*this);
        }

        const std::vector<T *> &parameters() const override { return params_; }
        const std::vector<std::string> &parameter_labels() const override
        {
            static const std::vector<std::string> labels{
                "spot", "rate", "div", "vol", "jump_intensity", "jump_prob_up",
                "jump_eta_up", "jump_eta_down"};
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

        /// Inverse CDF of the asymmetric double exponential, given a uniform
        /// u in (0,1) -- see this file's class comment for the derivation.
        T sample_jump(double u) const
        {
            using std::log;
            const double p_d = to_double(p_);
            if (u < 1.0 - p_d)
                return log(u / (1.0 - p_)) / eta2_;
            return -(log((1.0 - u) / p_) / eta1_);
        }

        static int sample_poisson(double u, double mean)
        {
            if (mean <= 0.0)
                return 0;
            constexpr int max_n = 200;
            double prob = std::exp(-mean);
            double cumulative = prob;
            int n = 0;
            while (u > cumulative && n < max_n)
            {
                ++n;
                prob *= mean / static_cast<double>(n);
                cumulative += prob;
            }
            return n;
        }

        void set_param_pointers()
        {
            params_ = {&s0_, &r_, &q_, &sigma_, &lambda_, &p_, &eta1_, &eta2_};
        }

        T s0_{}, r_{}, q_{}, sigma_{}, lambda_{}, p_{}, eta1_{}, eta2_{};
        int max_jumps_per_step_ = 10;
        TimeLine timeline_;
        std::vector<SampleDef> defline_;
        std::vector<Step> steps_;
        std::size_t sim_dim_ = 0;
        std::vector<T *> params_;
    };

} // namespace quantModeling

#endif // QM_MODELS_EQUITY_KOU_JUMP_DIFFUSION_SIM_MODEL_HPP
