#ifndef QM_MODELS_RATES_VASICEK_SIM_MODEL_HPP
#define QM_MODELS_RATES_VASICEK_SIM_MODEL_HPP

#include "quantModeling/aad/number.hpp"
#include "quantModeling/core/timegrid.hpp"
#include "quantModeling/core/types.hpp"
#include "quantModeling/models/simulation_model.hpp"

#include <cmath>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace quantModeling
{

    /**
     * @brief Vasicek (1977) short rate for the timeline simulation
     *        architecture -- issue #37's "rate models" item.
     *
     *   dr(t) = a (b - r(t)) dt + sigma dW(t)
     *
     * (Hull-White with a constant theta is the same process, b = theta/a --
     * see models/rates/hull_white.hpp's own doc comment -- so one model
     * covers both.)
     *
     * Unlike Black-Scholes, the quantity a rates product actually discounts
     * by is not `exp(r * t)` for a *fixed* r: r is itself the thing being
     * simulated, so the numeraire is the realized money-market account
     * N(t) = exp(integral of r(s) ds from 0 to t), a path-dependent
     * quantity. The point of Vasicek being an Ornstein-Uhlenbeck process is
     * that this is still simulable *exactly*, with no time-discretisation
     * error, because (r(t+dt), integral of r over the step) are *jointly
     * Gaussian* given r(t): both are linear functionals of the same
     * Brownian increment. Writing tau = dt and B(tau) = (1-e^{-a*tau})/a
     * (already used by VasicekModel's own closed-form bond price):
     *
     *   E[r(t+tau) | r(t)]       = r(t) e^{-a tau} + b (1 - e^{-a tau})
     *   Var[r(t+tau) | r(t)]     = sigma^2 (1 - e^{-2a tau}) / (2a)
     *   E[I(tau) | r(t)]         = b*tau + (r(t) - b) * B(tau)
     *   Var[I(tau) | r(t)]       = (sigma^2/a^2) (tau - 2B(tau) + (1-e^{-2a tau})/(2a))
     *   Cov[r(t+tau), I(tau)]    = (sigma^2/a) (B(tau) - (1-e^{-2a tau})/(2a))
     *
     * where I(tau) is this step's contribution to the integral. Two
     * independent standard normals z1, z2 per step then give an exact
     * joint draw via the usual Cholesky-style construction: r updates from
     * z1 alone, I updates from a mix of z1 (through the covariance) and z2
     * (the leftover, conditionally independent variance). `tests/
     * testVasicekSimModel.cpp` derives and checks these formulas directly
     * (Itô isometry on the two Wiener integrals involved), not just cites
     * them.
     *
     * Discounts at each sample date reuse the *same* closed form as
     * VasicekModel::zcb_price -- P(t, T | r_t) = A(tau) exp(-B(tau) r_t),
     * tau = T - t -- now T-typed, per issue #37: "discounts/forwards
     * deviendraient les formules fermées ... rendues T-typées". This is
     * exact regardless of the realized path, an affine-term-structure
     * property of Vasicek, not an approximation.
     *
     * `spots[0]` carries the current short rate (the one state variable a
     * rate product would reference from a script, the same role `spot()`
     * plays for equities). `forwards` is not populated: a "forward price
     * of the underlying" is not a meaningful concept for a rate itself the
     * way it is for a tradable spot, and no rate product needs it -- a
     * forward *rate* is a ratio of two discounts, already available from
     * `discounts`.
     */
    template <class T = Real>
    class VasicekSimModel final : public ISimulationModel<T>
    {
      public:
        /**
         * @param r0    Initial short rate r(0).
         * @param a     Mean-reversion speed (a > 0).
         * @param b     Long-term mean rate.
         * @param sigma Volatility (sigma > 0).
         */
        VasicekSimModel(T r0, T a, T b, T sigma)
            : r0_(r0), a_(a), b_(b), sigma_(sigma)
        {
            if (to_double(a_) <= 0.0)
                throw InvalidInput("VasicekSimModel: mean reversion a must be > 0");
            if (to_double(sigma_) <= 0.0)
                throw InvalidInput("VasicekSimModel: volatility sigma must be > 0");
            set_param_pointers();
        }

        /// See models/equity/bs_sim_model.hpp for why this exists.
        VasicekSimModel(const VasicekSimModel &other)
            : r0_(other.r0_), a_(other.a_), b_(other.b_), sigma_(other.sigma_), timeline_(other.timeline_), defline_(other.defline_), steps_(other.steps_), sim_dim_(other.sim_dim_)
        {
            set_param_pointers();
        }

        std::size_t n_underlyings() const override { return 1; }

        void init(const TimeLine &product_timeline,
                  const std::vector<SampleDef> &defline) override
        {
            using std::exp;
            using std::sqrt;

            timeline_ = canonical_timeline(product_timeline);
            defline_ = defline;

            steps_.clear();
            sim_dim_ = 0;
            Time t_prev = 0.0;
            for (const Time t : timeline_)
            {
                Step s;
                s.t = t;
                s.dt = t - t_prev;
                s.draws = (t > TIMELINE_EPS);
                if (s.draws)
                {
                    s.exp_a_dt = exp(-a_ * s.dt);
                    s.Bdt = (T(1.0) - s.exp_a_dt) / a_;
                    const T half_life_term = (T(1.0) - s.exp_a_dt * s.exp_a_dt) / (T(2.0) * a_);
                    const T var_r = sigma_ * sigma_ * half_life_term;
                    s.sd_r = sqrt(var_r);
                    const T var_I = (sigma_ * sigma_ / (a_ * a_)) *
                                    (T(s.dt) - T(2.0) * s.Bdt + half_life_term);
                    const T cov_rI = (sigma_ * sigma_ / a_) * (s.Bdt - half_life_term);
                    s.chol21 = cov_rI / s.sd_r;
                    s.sd_I_given_r = sqrt(var_I - s.chol21 * s.chol21 * var_r);
                    sim_dim_ += 2;
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

            T r = r0_;
            T I(0.0); // integral of r from 0 to the current sample date
            std::size_t g = 0;
            for (std::size_t i = 0; i < steps_.size(); ++i)
            {
                const Step &st = steps_[i];
                if (st.draws)
                {
                    const double z1 = gaussians[g++];
                    const double z2 = gaussians[g++];
                    const T mean_r = r * st.exp_a_dt + b_ * (T(1.0) - st.exp_a_dt);
                    const T mean_I = b_ * st.dt + (r - b_) * st.Bdt;
                    I = I + mean_I + st.chol21 * z1 + st.sd_I_given_r * z2;
                    r = mean_r + st.sd_r * z1;
                }

                Sample<T> &smp = path[i];
                smp.spots.assign(1, r);
                smp.numeraire = exp(I);

                const SampleDef &def = defline_[i];
                smp.discounts.resize(def.discount_mats.size());
                for (std::size_t k = 0; k < def.discount_mats.size(); ++k)
                    smp.discounts[k] = discount_between(st.t, def.discount_mats[k], r);
            }
        }

        std::unique_ptr<ISimulationModel<T>> clone() const override
        {
            return std::make_unique<VasicekSimModel<T>>(*this);
        }

        const std::vector<T *> &parameters() const override { return params_; }
        const std::vector<std::string> &parameter_labels() const override
        {
            static const std::vector<std::string> labels{"r0", "mean_reversion", "long_term_rate", "vol"};
            return labels;
        }

        /// P(0, T) -- exposed for tests to cross-check against
        /// VasicekModel::zcb_price without needing a whole simulation.
        T zcb_price(Time maturity) const { return discount_between(0.0, maturity, r0_); }

      private:
        struct Step
        {
            Time t = 0.0;
            Real dt = 0.0;
            bool draws = false;
            T exp_a_dt{};     // e^{-a*dt}
            T Bdt{};          // B(dt) = (1 - e^{-a*dt}) / a
            T sd_r{};         // sqrt(Var[r(t+dt) | r(t)])
            T chol21{};       // Cov(r, I) / sd_r
            T sd_I_given_r{}; // sqrt(Var[I] - chol21^2 * Var[r])
        };

        void set_param_pointers() { params_ = {&r0_, &a_, &b_, &sigma_}; }

        /// B(tau) = (1 - e^{-a*tau}) / a, T-typed -- same formula as
        /// VasicekModel::B, reused (not shared: that one is Real-only).
        T B_of(Time tau) const
        {
            using std::exp;
            return (T(1.0) - exp(-a_ * tau)) / a_;
        }

        /// ln A(tau), T-typed -- same formula as VasicekModel::lnA.
        T lnA_of(Time tau) const
        {
            const T Bval = B_of(tau);
            const T term1 = (Bval - T(tau)) * (a_ * a_ * b_ - T(0.5) * sigma_ * sigma_) / (a_ * a_);
            const T term2 = -sigma_ * sigma_ * Bval * Bval / (T(4.0) * a_);
            return term1 + term2;
        }

        /// P(t, T | r_t) = A(tau) exp(-B(tau) r_t), tau = T - t.
        T discount_between(Time t, Time maturity, const T &r_t) const
        {
            using std::exp;
            const Time tau = maturity - t;
            if (tau <= 0.0)
                return T(1.0);
            return exp(lnA_of(tau) - B_of(tau) * r_t);
        }

        T r0_, a_, b_, sigma_;
        TimeLine timeline_;
        std::vector<SampleDef> defline_;
        std::vector<Step> steps_;
        std::size_t sim_dim_ = 0;
        std::vector<T *> params_; // set_param_pointers() keeps this current
    };

} // namespace quantModeling

#endif // QM_MODELS_RATES_VASICEK_SIM_MODEL_HPP
