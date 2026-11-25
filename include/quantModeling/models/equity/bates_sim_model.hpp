#ifndef QM_MODELS_EQUITY_BATES_SIM_MODEL_HPP
#define QM_MODELS_EQUITY_BATES_SIM_MODEL_HPP

#include "quantModeling/aad/number.hpp"
#include "quantModeling/core/timegrid.hpp"
#include "quantModeling/core/types.hpp"
#include "quantModeling/models/equity/knock_out.hpp"
#include "quantModeling/models/equity/last_step.hpp"
#include "quantModeling/models/equity/path_steps.hpp"
#include "quantModeling/models/device_model.hpp"
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
     * @brief Bates (1996) — Heston stochastic volatility plus Merton-style
     *        compound-Poisson jumps — for the timeline simulation
     *        architecture.
     *
     *   dS/S = (r - q - lambda*k) dt + sqrt(v) dW1 + dJ
     *   dv   = kappa*(theta - v) dt + xi*sqrt(v) dW2,   dW1 dW2 = rho dt
     *
     * J is exactly MertonJumpDiffusionSimModel's compound Poisson (lognormal
     * jump sizes, compensator k = E[Y-1]) added on top of Heston's own
     * diffusion. v0/kappa/theta/xi/rho use the same names and meaning as
     * models/equity/heston.hpp's HestonParams.
     *
     * Unlike either piece on its own, this needs an internal fine grid
     * (add_monitoring_steps(), the same utility LocalVolSimModel uses):
     * variance has no exact transition density the way Black-Scholes's spot
     * does, so a step's own coefficients cannot be precomputed once in
     * init() and reused across paths -- v_plus below depends on that path's
     * own evolving variance, recomputed every step of every path. Full
     * truncation Euler (Lord, Koekkoek & van Dijk, "A Comparison of
     * Biased Simulation Schemes for Stochastic Volatility Models", 2010):
     * v_plus = max(v, 0) wherever v feeds a square root or the mean-
     * reversion drift, and v itself is left free to run negative rather
     * than reflected or absorbed -- simple, robust, and while it is not the
     * most accurate scheme in the literature (Andersen's QE scheme does
     * better), it is far simpler to get right, and correctness here is
     * checked against an independent, already-tested oracle (see the test
     * file: this model with lambda = 0 is exactly Heston, priced
     * semi-analytically by engines/analytic/heston_cos.hpp).
     *
     * v_plus is floored at a small positive epsilon rather than exactly 0:
     * aad::sqrt's own adjoint is 0.5/sqrt(x), which is +inf at x = 0 --
     * a value the full truncation scheme genuinely reaches on real
     * simulated paths whenever v wanders negative. An exact-zero clamp
     * would poison every downstream adjoint that touches sqrt_v_plus with
     * inf/NaN (observed directly: simulate_aad's aggregated std errors for
     * v0/kappa/theta/xi/rho came back NaN before this floor was added).
     * The floor is many orders of magnitude below any realistic variance,
     * so it changes no price to within Monte Carlo noise; it only removes
     * the derivative singularity at the boundary.
     *
     * Every one of s0/r/q/v0/kappa/theta/xi/rho/lambda/jump_mean/jump_vol is
     * T: with T = aad::Number, simulate_aad reports a genuine vega *per*
     * variance-process parameter (vol-of-vol, mean-reversion speed, the
     * long-run variance, the leverage correlation) alongside the jump risk
     * and the classic spot/rate/div greeks -- eleven parameters from one
     * run. As in the other jump and local-vol models here, *how many*
     * jumps occur is decided from a plain double draw (control flow is not
     * differentiated); max(v, 0)'s own kink, by contrast, *is* -- it uses
     * aad::max, which records a real (if kinked) subgradient, the same way
     * a local-vol grid lookup's clamped edges do.
     */
    template <class T = Real>
    class BatesSimModel final : public ISimulationModel<T>
    {
      public:
        BatesSimModel(T s0, T r, T q, T v0, T kappa, T theta, T xi, T rho,
                      T lambda, T jump_mean, T jump_vol, Time max_dt = 1.0 / 50.0)
            : s0_(s0), r_(r), q_(q), v0_(v0), kappa_(kappa), theta_(theta), xi_(xi), rho_(rho), lambda_(lambda), jump_mean_(jump_mean), jump_vol_(jump_vol), max_dt_(max_dt)
        {
            if (to_double(v0_) < 0.0)
                throw InvalidInput("BatesSimModel: v0 must be >= 0");
            if (to_double(kappa_) <= 0.0)
                throw InvalidInput("BatesSimModel: kappa must be > 0");
            if (to_double(theta_) <= 0.0)
                throw InvalidInput("BatesSimModel: theta must be > 0");
            if (to_double(xi_) <= 0.0)
                throw InvalidInput("BatesSimModel: xi must be > 0");
            if (to_double(rho_) < -1.0 || to_double(rho_) > 1.0)
                throw InvalidInput("BatesSimModel: rho must be in [-1, 1]");
            if (to_double(lambda_) < 0.0)
                throw InvalidInput("BatesSimModel: lambda must be >= 0");
            if (to_double(jump_vol_) < 0.0)
                throw InvalidInput("BatesSimModel: jump_vol must be >= 0");
            set_param_pointers();
        }

        /// See models/equity/bs_sim_model.hpp for why this exists.
        BatesSimModel(const BatesSimModel &other)
            : s0_(other.s0_), r_(other.r_), q_(other.q_), v0_(other.v0_), kappa_(other.kappa_), theta_(other.theta_), xi_(other.xi_), rho_(other.rho_), lambda_(other.lambda_), jump_mean_(other.jump_mean_), jump_vol_(other.jump_vol_), max_dt_(other.max_dt_), refinement_(other.refinement_), scheme_(other.scheme_), timeline_(other.timeline_), sim_timeline_(other.sim_timeline_), defline_(other.defline_), event_index_of_step_(other.event_index_of_step_), sim_dim_(other.sim_dim_), has_last_step_(other.has_last_step_), has_knock_out_(other.has_knock_out_)
        {
            set_param_pointers();
        }

        std::size_t n_underlyings() const override { return 1; }

        void init(const TimeLine &product_timeline,
                  const std::vector<SampleDef> &defline) override
        {
            refuse_bridge_survivals(defline, "BatesSimModel");
            has_last_step_ = mc::asks_last_step(defline);
            // Knock-outs (#369): the spot's step conditioned on survival,
            // lognormal given the variance -- Heston's, not with jumps.
            has_knock_out_ = mc::check_knock_outs(defline, "BatesSimModel");
            if (has_knock_out_ && !(to_double(lambda_) == 0.0))
                refuse_knock_outs(defline, "BatesSimModel with jumps");
            if (has_last_step_ && !(to_double(lambda_) == 0.0))
                throw InvalidInput("BatesSimModel: lastStepCall() / lastStepPut() / lastStepDigital() need a lognormal "
                                   "last step, and jumps make it a mixture (Heston, lambda = 0, has one)");
            timeline_ = canonical_timeline(product_timeline);
            sim_timeline_ = refine_steps(add_monitoring_steps(timeline_, max_dt_), refinement_);
            defline_ = defline;

            event_index_of_step_ = event_index_of_step(sim_timeline_, timeline_);

            // Every fine-grid step draws (add_monitoring_steps() only ever
            // returns points strictly after t = 0): diffusion, the vol
            // shock's independent component, the poisson-count uniform,
            // and the jump-sum magnitude.
            sim_dim_ = 4 * sim_timeline_.size();
        }

        /// Multilevel Monte Carlo (WP 26 §9.5): each max_dt step split into
        /// r -- without jumps only: coupling jump draws across levels is out of P4's first cut (WP 26 §8.3); with lambda = 0 (Heston) the jump draws are unused.
        bool set_refinement(int r) override
        {
            if (r < 1 || (r > 1 && !(to_double(lambda_) == 0.0)))
                return false;
            refinement_ = r;
            return true;
        }

        const TimeLine &sim_timeline() const override { return sim_timeline_; }
        std::size_t sim_dim() const override { return sim_dim_; }

        /// Every fine-grid step draws: spot and the variance shock's independent part; then the jump count and size.
        BrownianLayout brownian_layout() const override
        {
            return BrownianLayout{sim_timeline_, 2, 4};
        }

        void generate_path(std::span<const double> gaussians,
                           Scenario<T> &path) const override
        {
            using std::exp;
            using std::max;
            using std::sqrt;

            // k = E[Y - 1], the jump compensator -- deterministic, T-typed,
            // the same formula as MertonJumpDiffusionSimModel.
            const T k = exp(jump_mean_ + 0.5 * jump_vol_ * jump_vol_) - 1.0;

            T S = s0_;
            T v = v0_;
            // The last-step expectations (WP 27 §8 rung 1): an event before
            // any step pays its payoff of S0; the others are computed on the
            // step that reaches them (lognormal given v: no jumps, init()).
            if (has_last_step_ && !defline_.empty())
                mc::fill_last_step(defline_[0], path[0], S, T(0.0));
            Time t_prev = 0.0;
            std::size_t g = 0;
            for (std::size_t i = 0; i < sim_timeline_.size(); ++i)
            {
                const Time t = sim_timeline_[i];
                const Time dt = t - t_prev;
                const double sqdt = sqrt(dt);
                const T S_before = S;

                const double z_spot = gaussians[g++];
                const double z_vol_indep = gaussians[g++];
                const double u_poisson = norm_cdf(gaussians[g++]);
                const double z_jump = gaussians[g++];

                const T v_plus = max(v, kVarianceFloor);
                const T sqrt_v_plus = sqrt(v_plus);

                const int n_jumps = sample_poisson(u_poisson, to_double(lambda_) * dt);
                const std::ptrdiff_t reached = event_index_of_step_[i];
                const mc::KnockOutCorridor ko = has_knock_out_ && reached >= 0
                                                    ? mc::knock_out_corridor(defline_[static_cast<std::size_t>(reached)])
                                                    : mc::KnockOutCorridor{};
                bool stepped = false;
                if (ko.active()) // a knock-out at the date (#369); lambda = 0 (init)
                {
                    using std::log;
                    const mc::HestonParamsT<T> hp{v0_, kappa_, theta_, xi_, rho_};
                    T survival(1.0);
                    if constexpr (std::is_same_v<T, Real>)
                        if (scheme_ == mc::VarianceScheme::QE) // the variance first, then the spot's law given it
                        {
                            double mean, sd;
                            mc::qe_variance_step(hp, 1.0, r_, q_, v, dt, z_spot, mean, sd);
                            double z = z_vol_indep;
                            survival = mc::condition_on_survival(ko, log(S), mean, sd, z);
                            S = S * exp(mean + sd * z);
                            stepped = true;
                        }
                    if (!stepped) // Euler: z_spot drives the spot, then the variance through rho
                    {
                        const T mean = (r_ - q_ - 0.5 * v_plus) * dt;
                        const T sd = sqrt_v_plus * sqdt;
                        T z(z_spot);
                        survival = mc::condition_on_survival(ko, T(log(S)), mean, sd, z);
                        // heston_log_return's grouping, as the IR's step
                        S = S * exp(mc::heston_log_return(r_, q_, lambda_, k, v_plus, sqrt_v_plus, sqdt, dt, z));
                        mc::heston_variance_step(hp, v, v_plus, sqrt_v_plus, sqdt, dt, z, z_vol_indep);
                        stepped = true;
                    }
                    mc::write_knock_out(defline_[static_cast<std::size_t>(reached)],
                                        path[static_cast<std::size_t>(reached)], ko, survival);
                }
                if constexpr (std::is_same_v<T, Real>)
                    if (!stepped && scheme_ == mc::VarianceScheme::QE)
                    {
                        // QE for the diffusion (z_spot drives the variance,
                        // z_vol_indep the spot's independent part), the jump
                        // compensator in its drift, then the jumps.
                        mc::qe_step(mc::HestonParamsT<T>{v0_, kappa_, theta_, xi_, rho_}, 1.0, r_ - lambda_ * k, q_,
                                    S, v, dt, z_spot, z_vol_indep);
                        if (n_jumps > 0)
                        {
                            const double n = static_cast<double>(n_jumps);
                            S = S * exp(n * jump_mean_ + std::sqrt(n) * jump_vol_ * z_jump);
                        }
                        stepped = true;
                    }
                if (!stepped)
                {
                    T log_return = mc::heston_log_return(r_, q_, lambda_, k, v_plus, sqrt_v_plus, sqdt, dt, z_spot);
                    if (n_jumps > 0)
                    {
                        const double n = static_cast<double>(n_jumps);
                        using std::sqrt;
                        log_return = log_return + (n * jump_mean_ + sqrt(n) * jump_vol_ * z_jump);
                    }
                    S = S * exp(log_return);
                    mc::heston_variance_step(mc::HestonParamsT<T>{v0_, kappa_, theta_, xi_, rho_}, v, v_plus,
                                             sqrt_v_plus, sqdt, dt, z_spot, z_vol_indep);
                }
                t_prev = t;

                const std::ptrdiff_t event = event_index_of_step_[i];
                if (event < 0)
                    continue;
                if (has_last_step_) // given v: lognormal, forward S e^{(r - q) dt}, log-variance v+ dt
                    mc::fill_last_step(defline_[static_cast<std::size_t>(event)], path[static_cast<std::size_t>(event)],
                                       T(S_before * exp((r_ - q_) * dt)), T(v_plus * dt));

                Sample<T> &smp = path[static_cast<std::size_t>(event)];
                smp.spots.assign(1, S);
                smp.numeraire = exp(r_ * t);

                const SampleDef &def = defline_[static_cast<std::size_t>(event)];
                if (def.inst_vol) // the diffusion's vol from here (jumps aside): sqrt(v_t+)
                    smp.inst_vol = sqrt(max(v, kVarianceFloor));
                smp.discounts.resize(def.discount_mats.size());
                for (std::size_t m = 0; m < def.discount_mats.size(); ++m)
                    smp.discounts[m] = exp(-r_ * (def.discount_mats[m] - t));

                smp.forwards.resize(def.forward_mats.size());
                for (std::size_t m = 0; m < def.forward_mats.size(); ++m)
                {
                    const Time mat = def.forward_mats[m];
                    smp.forwards[m] = S * exp((r_ - q_) * (mat - t));
                }
            }
        }

        std::unique_ptr<ISimulationModel<T>> clone() const override
        {
            return std::make_unique<BatesSimModel<T>>(*this);
        }

        bool set_variance_scheme(mc::VarianceScheme scheme) override
        {
            scheme_ = scheme;
            return true;
        }

        bool describe_device(DeviceModel &d) const override
        {
            if constexpr (!std::is_same_v<T, Real>)
                return false;
            else
            {
                if (lambda_ != 0.0)
                    return false; // jumps stay on the CPU
                d = DeviceModel{};
                d.kind = DeviceModel::Kind::Heston;
                d.r = r_;
                d.q = q_;
                d.s0 = {s0_};
                d.heston = {v0_, kappa_, theta_, xi_, rho_};
                d.jump_k = std::exp(jump_mean_ + 0.5 * jump_vol_ * jump_vol_) - 1.0;
                d.factors = 2;
                d.stride = 4; // spot, variance, then the (unused) jump count and size
                d.variance_scheme = scheme_;
                d.t = sim_timeline_;
                d.draws.assign(sim_timeline_.size(), 1);
                d.event.clear();
                for (const std::ptrdiff_t e : event_index_of_step_)
                    d.event.push_back(static_cast<int>(e));
                return true;
            }
        }

        /// The spot's log-return carries the jump compensator -lambda k, so
        /// S(t) / N(t) is a martingale on the grid: E = S0 e^{-q t}.
        bool deflated_spot_mean(std::size_t asset, Time t, Real &mean) const override
        {
            if (asset != 0)
                return false;
            mean = to_double(s0_) * std::exp(-to_double(q_) * t);
            return true;
        }

        const std::vector<T *> &parameters() const override { return params_; }
        const std::vector<std::string> &parameter_labels() const override
        {
            static const std::vector<std::string> labels{
                "spot", "rate", "div", "v0",
                "kappa", "theta", "xi", "rho",
                "jump_intensity", "jump_mean", "jump_vol"};
            return labels;
        }

      private:
        static constexpr double kVarianceFloor = 1e-10;

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
            params_ = {&s0_, &r_, &q_, &v0_, &kappa_,
                       &theta_, &xi_, &rho_, &lambda_, &jump_mean_,
                       &jump_vol_};
        }

        T s0_{}, r_{}, q_{}, v0_{}, kappa_{}, theta_{}, xi_{}, rho_{}, lambda_{},
            jump_mean_{}, jump_vol_{};
        Time max_dt_;
        int refinement_ = 1;                                    // multilevel Monte Carlo: each max_dt step split in r
        mc::VarianceScheme scheme_ = mc::VarianceScheme::Euler; // price runs only (#317)

        TimeLine timeline_;     // the product's own event dates
        TimeLine sim_timeline_; // timeline_ plus internal Euler steps
        std::vector<SampleDef> defline_;
        std::vector<std::ptrdiff_t> event_index_of_step_;
        std::size_t sim_dim_ = 0;
        bool has_last_step_ = false; ///< the defline asks for a lastStep*() expectation
        bool has_knock_out_ = false; ///< the defline has a knock-out (#369)
        std::vector<T *> params_;
    };

} // namespace quantModeling

#endif // QM_MODELS_EQUITY_BATES_SIM_MODEL_HPP
