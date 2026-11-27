#ifndef QM_MODELS_EQUITY_ROUGH_BERGOMI_SIM_MODEL_HPP
#define QM_MODELS_EQUITY_ROUGH_BERGOMI_SIM_MODEL_HPP

#include "quantModeling/aad/number.hpp"
#include "quantModeling/core/timegrid.hpp"
#include "quantModeling/core/types.hpp"
#include "quantModeling/engines/mc/rough_bergomi_surface.hpp"
#include "quantModeling/models/equity/curve_carry.hpp"
#include "quantModeling/models/equity/last_step.hpp"
#include "quantModeling/models/equity/rough_bergomi.hpp"
#include "quantModeling/models/simulation_model.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace quantModeling
{

    /**
     * @brief Rough Bergomi (Bayer-Friz-Gatheral 2016), hybrid scheme
     *        (Bennedsen-Lunde-Pakkanen 2017), for the timeline simulation
     *        architecture -- issue #37's roughest item, and the roadmap's
     *        stated differentiator (etc/roadmap.md §1c).
     *
     *   dS/S  = (r - q) dt + sqrt(V_t) dZ_t
     *   V_t   = xi0 * exp(eta*W^alpha_t - 0.5*eta^2*t^(2*alpha+1))
     *   dZ_t  = rho*dW1_t + sqrt(1-rho^2) dW2_t
     *
     * where W^alpha is the Riemann-Liouville Volterra process driven by W1
     * (see models/equity/rough_bergomi.hpp) and alpha = H - 1/2. This is
     * engines/mc/rough_bergomi_hybrid.hpp's algorithm exactly -- same
     * near/far split, same b_k weights, same O(n^2) direct convolution
     * (that header's doc comment explains why, unchanged here) -- carried
     * into the AAD architecture, with one deliberate scope cut and one
     * deliberate non-differentiable parameter, both load-bearing rather
     * than incidental:
     *
     * **H (the Hurst exponent) is Real, not T** -- the same treatment
     * SABRSimModel gives beta, and for the same kind of reason, not just
     * convenience: in practice a desk estimates H once from historical
     * realized volatility (Gatheral-Jaisson-Rosenbaum's own empirical
     * finding motivating this whole model) and treats it as structural,
     * not recalibrated per option the way eta/rho are. It also makes AAD
     * here tractable at all: every hybrid-scheme weight (b_k, the near-term
     * Cholesky factors, dt^alpha) depends only on H and the (uniform) step
     * size, so with H fixed they are plain doubles, precomputed once in
     * init(), and the O(n^2) far-field convolution -- by far the most
     * expensive part of a path -- runs in pure double arithmetic with zero
     * tape nodes. T-dependence enters only where eta rescales the
     * already-computed real Volterra value into a variance level, and
     * where rho mixes two already-computed real Brownian shocks -- O(1)
     * tape work per step, the same order every other model here costs.
     *
     * **Only single-maturity products are supported.** The hybrid scheme's
     * weights are derived for a *uniform* grid of step dt = T/n_steps; the
     * generic add_monitoring_steps() every other model here uses can
     * produce a non-uniform grid once a product has more than one event
     * date (a different, possibly uneven, step size in each gap between
     * events), which would silently invalidate every precomputed weight.
     * This is exactly engines/mc/rough_bergomi_hybrid.hpp's own existing
     * scope (one European vanilla, one maturity) carried over unchanged,
     * not a new limitation this port introduces -- init() throws rather
     * than silently mis-simulate a multi-date product.
     *
     * V_t is a Wick exponential (xi0 * exp(...)) and therefore always
     * strictly positive for xi0 > 0: unlike Heston/Bates/SLV's CIR-type
     * variance, there is no zero boundary to floor and no sqrt(0) adjoint
     * singularity to guard against here.
     */
    template <class T = Real>
    class RoughBergomiSimModel final : public ISimulationModel<T>
    {
      public:
        /**
         * @param s0, r, q   Spot, rate, continuous dividend yield.
         * @param xi0        Flat forward variance V_0 (no term structure --
         *                   the same simplification Heston's v0 already is).
         * @param eta        Vol-of-vol.
         * @param rho        Correlation between the price- and variance-
         *                   driving Brownian motions.
         * @param H          Hurst exponent, in (0, 1/2). Structural, not T
         *                   -- see the class doc comment.
         * @param n_steps    Number of uniform hybrid-scheme steps over the
         *                   product's single event date.
         */
        RoughBergomiSimModel(T s0, T r, T q, T xi0, T eta, T rho, Real H,
                             std::size_t n_steps = 100)
            : s0_(s0), r_(r), q_(q), xi0_(xi0), eta_(eta), rho_(rho), H_(H), n_steps_(n_steps)
        {
            if (to_double(xi0_) <= 0.0)
                throw InvalidInput("RoughBergomiSimModel: xi0 must be > 0");
            if (to_double(eta_) <= 0.0)
                throw InvalidInput("RoughBergomiSimModel: eta must be > 0");
            if (to_double(rho_) < -1.0 || to_double(rho_) > 1.0)
                throw InvalidInput("RoughBergomiSimModel: rho must be in [-1, 1]");
            if (H_ <= 0.0 || H_ >= 0.5)
                throw InvalidInput("RoughBergomiSimModel: H must be in (0, 1/2)");
            if (n_steps_ == 0)
                throw InvalidInput("RoughBergomiSimModel: n_steps must be >= 1");
            set_param_pointers();
        }

        /**
         * @brief The calibrated form (etc/roadmap.md §1c): a forward variance
         *        curve ξ0(t) instead of a flat ξ0, and any number of event
         *        dates. The hybrid scheme runs on a uniform grid of
         *        1/steps_per_year up to the last event, and each event reads
         *        the path at its nearest grid date (at most half a step
         *        away) -- the uniform grid the scheme's weights need, kept
         *        for multi-date products (scripts: barriers, autocalls).
         *        ξ0(t) is market data here, not a differentiable parameter.
         */
        RoughBergomiSimModel(T s0, T r, T q, ForwardVarianceCurve xi, T eta, T rho, Real H, int steps_per_year)
            : s0_(s0), r_(r), q_(q), xi0_(T(xi(0.0))), eta_(eta), rho_(rho), H_(H), n_steps_(1), xi_curve_(std::move(xi)), steps_per_year_(steps_per_year)
        {
            if (to_double(eta_) < 0.0)
                throw InvalidInput("RoughBergomiSimModel: eta must be >= 0");
            if (to_double(rho_) < -1.0 || to_double(rho_) > 1.0)
                throw InvalidInput("RoughBergomiSimModel: rho must be in [-1, 1]");
            if (H_ <= 0.0 || H_ >= 0.5)
                throw InvalidInput("RoughBergomiSimModel: H must be in (0, 1/2)");
            if (steps_per_year_ < 1 || xi_curve_.xi.empty())
                throw InvalidInput("RoughBergomiSimModel: need steps_per_year >= 1 and a forward variance curve");
            set_param_pointers();
        }

        /// See models/equity/bs_sim_model.hpp for why this exists.
        RoughBergomiSimModel(const RoughBergomiSimModel &other)
            : s0_(other.s0_), r_(other.r_), q_(other.q_), xi0_(other.xi0_), eta_(other.eta_), rho_(other.rho_), H_(other.H_), n_steps_(other.n_steps_), xi_curve_(other.xi_curve_), steps_per_year_(other.steps_per_year_), event_step_(other.event_step_), xi_at_(other.xi_at_), timeline_(other.timeline_), defline_(other.defline_), dt_(other.dt_), alpha_(other.alpha_), l11_(other.l11_), l21_(other.l21_), l22_(other.l22_), dt_pow_alpha_(other.dt_pow_alpha_), sqrt_2alpha_plus_1_(other.sqrt_2alpha_plus_1_), b_pow_alpha_(other.b_pow_alpha_), compensator_(other.compensator_), sqrt_dt_(other.sqrt_dt_), half_dt_(other.half_dt_), sim_dim_(other.sim_dim_), carry_(other.carry_)
        {
            set_param_pointers();
        }

        std::size_t n_underlyings() const override { return 1; }

        /// The forward curve's carry over each grid step and its discounting
        /// (WP 32 F2), r and q as parallel shifts over it (curve_carry.hpp).
        bool set_forward_curve(const ForwardCurve &curve) override
        {
            carry_.emplace(curve);
            set_param_pointers(); // the pillar shifts (WP 32 F3)
            return true;
        }

        void init(const TimeLine &product_timeline,
                  const std::vector<SampleDef> &defline) override
        {
            refuse_bridge_survivals(defline, "RoughBergomiSimModel"); // lastStep*: write_sample
            refuse_knock_outs(defline, "RoughBergomiSimModel");
            timeline_ = canonical_timeline(product_timeline);
            if (timeline_.empty())
                throw InvalidInput("RoughBergomiSimModel: the product has no event date");
            if (steps_per_year_ == 0 && timeline_.size() != 1)
                throw InvalidInput(
                    "RoughBergomiSimModel: the flat-xi0 form prices single-maturity "
                    "products only -- use the forward-variance-curve constructor, "
                    "which reads several dates off one uniform grid");
            defline_ = defline;

            const Real Tm = timeline_.back();
            if (steps_per_year_ > 0)
                n_steps_ = static_cast<std::size_t>(
                    std::max(1.0, std::ceil(Tm * static_cast<Real>(steps_per_year_) - 1e-9)));
            dt_ = Tm / static_cast<Real>(n_steps_);
            event_step_.clear();
            for (const Time t : timeline_)
                event_step_.push_back(static_cast<std::size_t>(
                    std::clamp<double>(std::round(t / dt_), 1.0, static_cast<double>(n_steps_))));
            xi_at_.assign(n_steps_ + 1, 0.0);
            for (std::size_t i = 0; i <= n_steps_; ++i)
                xi_at_[i] = xi_curve_.xi.empty() ? 1.0 : xi_curve_(static_cast<Real>(i) * dt_);
            alpha_ = H_ - 0.5;

            const Real sigma11 = dt_;
            const Real sigma12 = std::pow(dt_, alpha_ + 1.0) / (alpha_ + 1.0);
            const Real sigma22 = std::pow(dt_, 2.0 * alpha_ + 1.0) / (2.0 * alpha_ + 1.0);
            l11_ = std::sqrt(sigma11);
            l21_ = sigma12 / l11_;
            l22_ = std::sqrt(std::max(sigma22 - l21_ * l21_, Real(0.0)));
            dt_pow_alpha_ = std::pow(dt_, alpha_);
            sqrt_2alpha_plus_1_ = std::sqrt(2.0 * alpha_ + 1.0);

            // b_k weights (McCrickerd & Pakkanen eq. 1.3): depend on alpha
            // (hence H) and k only, not on dt -- see
            // engines/mc/rough_bergomi_hybrid.cpp's hybrid_b_weights, ported
            // verbatim.
            b_pow_alpha_.assign(n_steps_ + 1, 0.0);
            for (std::size_t k = 2; k <= n_steps_; ++k)
            {
                const Real kk = static_cast<Real>(k);
                const Real base = (std::pow(kk, alpha_ + 1.0) - std::pow(kk - 1.0, alpha_ + 1.0)) /
                                  (alpha_ + 1.0);
                b_pow_alpha_[k] = std::pow(std::pow(base, 1.0 / alpha_), alpha_);
            }

            // t_i^(2 alpha + 1), the Wick exponential's compensator, and
            // sqrt(dt): the same values the path loop used to recompute at
            // every step of every path (a pow per step), now read off a
            // table -- identical bits.
            compensator_.assign(n_steps_ + 1, 0.0);
            for (std::size_t i = 1; i <= n_steps_; ++i)
                compensator_[i] = std::pow(static_cast<Real>(i) * dt_, 2.0 * alpha_ + 1.0);
            sqrt_dt_ = std::sqrt(dt_);
            half_dt_ = 0.5 * dt_;
            if (carry_)
            {
                carry_->set_uniform_steps(n_steps_, dt_);
                carry_->set_events(timeline_, defline_);
            }

            sim_dim_ = 3 * n_steps_;
        }

        const TimeLine &sim_timeline() const override { return timeline_; }
        std::size_t sim_dim() const override { return sim_dim_; }

        void generate_path(std::span<const double> gaussians,
                           Scenario<T> &path) const override
        {
            using std::exp;
            using std::max;
            using std::sqrt;

            // The W1 increments are known before the path is stepped (each is
            // its own gaussian times l11), so every far-field sum is computed
            // first, several at once (far_field_sums), each in its original
            // order: identical bits, without the serial dependency of one
            // sum's additions on the step loop.
            thread_local std::vector<Real> dW1, far;
            dW1.assign(n_steps_ + 1, 0.0);
            far.assign(n_steps_ + 1, 0.0);
            for (std::size_t i = 1; i <= n_steps_; ++i)
                dW1[i] = l11_ * gaussians[3 * (i - 1)];
            far_field_sums(dW1.data(), far.data());

            T S = s0_;
            // V_0: ξ0 flat, or the curve's first value.
            T v_prev = xi_curve_.xi.empty() ? xi0_ : T(xi_at_[0]);
            // Per path, not per step: on the AAD tape each is one node rather
            // than one a step.
            const T rho_bar = sqrt(max(1.0 - rho_ * rho_, 0.0));
            const T half_eta2 = 0.5 * eta_ * eta_;
            const T drift = (r_ - q_) * dt_;
            std::size_t next_event = 0;
            std::size_t g = 0;

            for (std::size_t i = 1; i <= n_steps_; ++i)
            {
                const double z_a = gaussians[g++];
                const double z_b = gaussians[g++];
                const double z2 = gaussians[g++];

                const Real dw1_i = dW1[i];
                const Real y_i = l21_ * z_a + l22_ * z_b;

                // Advance S over [t_{i-1}, t_i] using v_prev = V(t_{i-1})
                // (left-point Euler on the log-price -- same convention as
                // the double-only engine; the hard, non-Markovian part is
                // entirely in simulating V, not in this step).
                const T dz_i = rho_ * dw1_i + rho_bar * (sqrt_dt_ * z2);
                // v * (dt / 2): the same bits as 0.5 * v * dt (a halving is
                // exact), one tape node instead of two.
                if (carry_) // the curve's carry over the step, ln F(t_i) / F(t_i-1)
                    S = S * exp(sqrt(v_prev) * dz_i - v_prev * half_dt_ + (drift + carry_->step_log_carry(i - 1, dt_)));
                else
                    S = S * exp(sqrt(v_prev) * dz_i - v_prev * half_dt_ + drift);

                const Real w_tilde_i = sqrt_2alpha_plus_1_ * (y_i + dt_pow_alpha_ * far[i]);

                // The one place eta enters: rescaling an already-computed
                // real Volterra value into the next step's variance level.
                // The curve's ξ0(t_i) is market data, a double: multiplying
                // by it records no constant node on the tape.
                const T wick = exp(eta_ * w_tilde_i - half_eta2 * compensator_[i]);
                v_prev = xi_curve_.xi.empty() ? T(xi0_ * wick) : T(xi_at_[i] * wick);

                while (next_event < event_step_.size() && event_step_[next_event] == i)
                    write_sample(path, next_event++, S);
            }
        }

        std::unique_ptr<ISimulationModel<T>> clone() const override
        {
            return std::make_unique<RoughBergomiSimModel<T>>(*this);
        }

        const std::vector<T *> &parameters() const override { return params_; }
        const std::vector<std::string> &parameter_labels() const override
        {
            return labels_;
        }

        Real H() const { return H_; }

      private:
        /**
         * far[i] = sum_{k=2..i} b_k dW1[i-k+1] for i = 1..n (McCrickerd &
         * Pakkanen eq. 1.3's discrete convolution), O(n^2) per path and pure
         * double arithmetic (H fixed). Each sum runs over k in increasing
         * order from 0.0, exactly as the step loop summed it, but kLanes
         * consecutive i are carried together: at a given k their terms read
         * contiguous dW1 entries and the chains are independent, which the
         * compiler vectorises without reassociating any sum.
         *
         * Measured 2026-11-16 (server, one thread, a one-year path on the
         * daily grid, with the compensator table): generate_path 45.1 ->
         * 19.9 us a path, the same bits.
         */
        void far_field_sums(const Real *dW1, Real *far) const
        {
            constexpr std::size_t kLanes = 8;
            const Real *b = b_pow_alpha_.data();
            const std::size_t n = n_steps_;
            far[1] = 0.0;
            for (std::size_t i0 = 2; i0 <= n; i0 += kLanes)
            {
                const std::size_t lanes = std::min(kLanes, n - i0 + 1);
                Real acc[kLanes] = {};
                // Every lane has the terms k = 2..i0.
                for (std::size_t k = 2; k <= i0; ++k)
                {
                    const Real bk = b[k];
                    const Real *w = dW1 + (i0 - k + 1);
                    for (std::size_t l = 0; l < kLanes; ++l)
                        acc[l] += bk * w[l < lanes ? l : 0];
                }
                // Lane l carries on with k = i0 + 1..i0 + l, in order.
                for (std::size_t l = 1; l < lanes; ++l)
                    for (std::size_t k = i0 + 1; k <= i0 + l; ++k)
                        acc[l] += b[k] * dW1[i0 + l - k + 1];
                for (std::size_t l = 0; l < lanes; ++l)
                    far[i0 + l] = acc[l];
            }
        }

        /// Event e reads the path at its grid date; rates and forwards at the
        /// event's own date.
        void write_sample(Scenario<T> &path, std::size_t e, const T &S) const
        {
            using std::exp;
            const Time t = timeline_[e];
            Sample<T> &smp = path[e];
            smp.spots.assign(1, S);
            const SampleDef &def = defline_[e];
            // lastStep*() (WP 27 §8 rung 1): this model has no closed form
            // over its last step, so the slot is the payoff of the simulated
            // spot -- an unbiased estimate of that expectation, the hard
            // payoff the primitive replaces (issue #363: the library's
            // digital reads lastStepDigital under every model).
            if (!def.monitors.empty())
                mc::fill_last_step(def, smp, S, T(0.0));
            if (carry_)
            {
                carry_->write_rates(def, e, t, S, r_, q_, smp);
                return;
            }
            smp.numeraire = exp(r_ * t);
            smp.discounts.resize(def.discount_mats.size());
            for (std::size_t m = 0; m < def.discount_mats.size(); ++m)
                smp.discounts[m] = exp(-r_ * (def.discount_mats[m] - t));
            smp.forwards.resize(def.forward_mats.size());
            for (std::size_t m = 0; m < def.forward_mats.size(); ++m)
                smp.forwards[m] = S * exp((r_ - q_) * (def.forward_mats[m] - t));
        }

        void set_param_pointers()
        {
            params_ = {&s0_, &r_, &q_, &xi0_, &eta_, &rho_};
            labels_ = {"spot", "rate", "div", "xi0", "eta", "rho"};
            if (carry_)
                carry_->append_parameters(params_, labels_);
        }

        T s0_{}, r_{}, q_{}, xi0_{}, eta_{}, rho_{};
        Real H_;
        std::size_t n_steps_;
        ForwardVarianceCurve xi_curve_; ///< empty: the flat xi0_ form
        int steps_per_year_ = 0;        ///< 0: n_steps_ over one maturity
        std::vector<std::size_t> event_step_;
        std::vector<Real> xi_at_;

        TimeLine timeline_;
        std::vector<SampleDef> defline_;

        Real dt_ = 0.0, alpha_ = 0.0;
        Real l11_ = 0.0, l21_ = 0.0, l22_ = 0.0;
        Real dt_pow_alpha_ = 0.0, sqrt_2alpha_plus_1_ = 0.0;
        std::vector<Real> b_pow_alpha_;
        std::vector<Real> compensator_;
        Real sqrt_dt_ = 0.0, half_dt_ = 0.0;
        std::size_t sim_dim_ = 0;
        std::optional<ModelCurve<T>> carry_; ///< WP 32: r_, q_ are then shifts over it

        std::vector<T *> params_;
        std::vector<std::string> labels_;
    };

} // namespace quantModeling

#endif // QM_MODELS_EQUITY_ROUGH_BERGOMI_SIM_MODEL_HPP
