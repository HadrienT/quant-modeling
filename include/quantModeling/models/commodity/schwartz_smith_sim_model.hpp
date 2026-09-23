#ifndef QM_MODELS_COMMODITY_SCHWARTZ_SMITH_SIM_MODEL_HPP
#define QM_MODELS_COMMODITY_SCHWARTZ_SMITH_SIM_MODEL_HPP

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
     * @brief Schwartz-Smith (2000) two-factor commodity model for the
     *        timeline simulation architecture -- issue #37's commodities
     *        item ("retour à la moyenne / saisonnalité").
     *
     *   ln(S_t) = chi_t + xi_t
     *   d(chi_t) = -kappa*chi_t dt + sigma_chi dW_chi     (short-term deviation, mean-reverts to 0)
     *   d(xi_t)  = mu_xi dt + sigma_xi dW_xi              (equilibrium level, a drifted random walk)
     *   dW_chi dW_xi = rho dt
     *
     * This is Schwartz's original mean-reverting model (his "Model 1", one
     * factor, ln(S) itself an Ornstein-Uhlenbeck process) generalised to
     * two factors the way the literature actually uses it: chi captures
     * short-term deviations that mean-revert (the empirical fact commodity
     * spot prices spike then decay -- weather, a refinery outage), xi
     * captures the slower-moving equilibrium level. The one-factor case is
     * exactly kappa -> the whole thing collapses to chi alone with
     * sigma_xi = mu_xi = rho = 0.
     *
     * Both factors are affine-Gaussian (chi is OU, xi is arithmetic
     * Brownian motion with drift), driven by *correlated* Brownian
     * motions -- so, exactly as for VasicekSimModel's (r, integral of r),
     * (chi_{t+tau}, xi_{t+tau}) given (chi_t, xi_t) is jointly Gaussian in
     * closed form, and this simulates *exactly*, with zero time-
     * discretisation error regardless of step size:
     *
     *   E[chi_{t+tau} | chi_t]  = chi_t * e^{-kappa*tau}
     *   Var[chi_{t+tau} | chi_t] = sigma_chi^2 (1 - e^{-2*kappa*tau}) / (2*kappa)
     *   E[xi_{t+tau} | xi_t]     = xi_t + mu_xi*tau
     *   Var[xi_{t+tau} | xi_t]   = sigma_xi^2 * tau
     *   Cov[chi_{t+tau}, xi_{t+tau} | chi_t] = rho*sigma_chi*sigma_xi*(1-e^{-kappa*tau})/kappa
     *
     * (the covariance by the same Ito-isometry cross-term argument
     * VasicekSimModel's doc comment uses: chi's Wiener integral has kernel
     * sigma_chi*e^{-kappa(tau-s)}, xi's is the constant sigma_xi, so
     * Cov = rho*sigma_chi*sigma_xi * integral_0^tau e^{-kappa(tau-s)} ds).
     * Two independent normals per step then give an exact joint draw via
     * the same Cholesky-style construction VasicekSimModel uses -- so,
     * unlike that model, there is no numeraire complication here needing a
     * *third* jointly-Gaussian quantity: the discount rate r is a separate,
     * deterministic input (this model prices the commodity, not the money-
     * market account), so the usual numeraire = exp(r*t) applies directly,
     * and (unlike SLV/RoughBergomi/Bates) no internal fine grid is needed
     * at all -- one exact step per product event date, the same pattern
     * BlackScholesSimModel and VasicekSimModel use.
     *
     * **What this deliberately leaves out of scope, both real Schwartz-
     * Smith extensions**: seasonality (an added deterministic function of
     * calendar time to xi, orthogonal to everything above -- a real
     * extension, not attempted here) and the three-factor model that adds
     * a third, stochastic short-rate factor correlated with the other two
     * (which would need re-deriving a 3x3 joint-Gaussian system combining
     * this model with VasicekSimModel's own -- a chantier of its own).
     *
     * **A parametrisation note, not a martingale claim**: mu_xi here is
     * whatever risk-neutral drift a calibration would produce for the
     * equilibrium level: this class does not itself enforce
     * E[S_T] = S_0 * e^{r*T} (there is no continuous dividend/convenience-
     * yield parameter to absorb the difference the way q does for
     * Black-Scholes -- kappa's mean-reversion of chi plays that role
     * implicitly and does not, in general, make discounted spot an exact
     * martingale for a constant mu_xi at every horizon simultaneously).
     * What *is* exact and is what the tests check: the closed-form mean
     * and variance of ln(S_T) above.
     */
    template <class T = Real>
    class SchwartzSmithSimModel final : public ISimulationModel<T>
    {
      public:
        /**
         * @param s0        Spot price, s0 > 0.
         * @param r         Risk-free rate (deterministic, flat).
         * @param chi0      Initial short-term deviation (0 = "currently at
         *                  equilibrium", the common default, but a genuine
         *                  differentiable state -- it is not fixed
         *                  structural data the way, say, SABR's beta is).
         * @param kappa     Mean-reversion speed of chi, kappa > 0.
         * @param sigma_chi Volatility of chi, sigma_chi > 0.
         * @param mu_xi     Drift of the equilibrium level xi.
         * @param sigma_xi  Volatility of xi, sigma_xi > 0.
         * @param rho       Correlation between the two driving Brownian
         *                  motions, in [-1, 1].
         */
        SchwartzSmithSimModel(T s0, T r, T chi0, T kappa, T sigma_chi, T mu_xi,
                              T sigma_xi, T rho)
            : s0_(s0), r_(r), chi0_(chi0), kappa_(kappa), sigma_chi_(sigma_chi), mu_xi_(mu_xi), sigma_xi_(sigma_xi), rho_(rho)
        {
            if (to_double(s0_) <= 0.0)
                throw InvalidInput("SchwartzSmithSimModel: s0 must be > 0");
            if (to_double(kappa_) <= 0.0)
                throw InvalidInput("SchwartzSmithSimModel: kappa must be > 0");
            if (to_double(sigma_chi_) <= 0.0)
                throw InvalidInput("SchwartzSmithSimModel: sigma_chi must be > 0");
            if (to_double(sigma_xi_) <= 0.0)
                throw InvalidInput("SchwartzSmithSimModel: sigma_xi must be > 0");
            if (to_double(rho_) < -1.0 || to_double(rho_) > 1.0)
                throw InvalidInput("SchwartzSmithSimModel: rho must be in [-1, 1]");
            set_param_pointers();
        }

        /// See models/equity/bs_sim_model.hpp for why this exists.
        SchwartzSmithSimModel(const SchwartzSmithSimModel &other)
            : s0_(other.s0_), r_(other.r_), chi0_(other.chi0_), kappa_(other.kappa_), sigma_chi_(other.sigma_chi_), mu_xi_(other.mu_xi_), sigma_xi_(other.sigma_xi_), rho_(other.rho_), timeline_(other.timeline_), defline_(other.defline_), steps_(other.steps_), sim_dim_(other.sim_dim_)
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
                    s.exp_k_dt = exp(-kappa_ * s.dt);
                    const T var_chi = sigma_chi_ * sigma_chi_ *
                                      (T(1.0) - s.exp_k_dt * s.exp_k_dt) / (T(2.0) * kappa_);
                    s.sd_chi = sqrt(var_chi);
                    const T var_xi = sigma_xi_ * sigma_xi_ * s.dt;
                    const T cov = rho_ * sigma_chi_ * sigma_xi_ * (T(1.0) - s.exp_k_dt) / kappa_;
                    s.chol21 = cov / s.sd_chi;
                    s.sd_xi_given_chi = sqrt(var_xi - s.chol21 * s.chol21 * var_chi);
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
            using std::log;

            T chi = chi0_;
            T xi = log(s0_) - chi0_;
            std::size_t g = 0;
            for (std::size_t i = 0; i < steps_.size(); ++i)
            {
                const Step &st = steps_[i];
                if (st.draws)
                {
                    const double z1 = gaussians[g++];
                    const double z2 = gaussians[g++];
                    const T mean_chi = chi * st.exp_k_dt;
                    const T mean_xi = xi + mu_xi_ * st.dt;
                    const T chi_new = mean_chi + st.sd_chi * z1;
                    xi = mean_xi + st.chol21 * z1 + st.sd_xi_given_chi * z2;
                    chi = chi_new;
                }

                Sample<T> &smp = path[i];
                smp.spots.assign(1, exp(chi + xi));
                smp.numeraire = exp(r_ * st.t);

                const SampleDef &def = defline_[i];
                smp.discounts.resize(def.discount_mats.size());
                for (std::size_t m = 0; m < def.discount_mats.size(); ++m)
                    smp.discounts[m] = exp(-r_ * (def.discount_mats[m] - st.t));
            }
        }

        std::unique_ptr<ISimulationModel<T>> clone() const override
        {
            return std::make_unique<SchwartzSmithSimModel<T>>(*this);
        }

        const std::vector<T *> &parameters() const override { return params_; }
        const std::vector<std::string> &parameter_labels() const override
        {
            static const std::vector<std::string> labels{
                "spot", "rate", "chi0", "kappa", "sigma_chi", "mu_xi", "sigma_xi", "rho"};
            return labels;
        }

      private:
        struct Step
        {
            Time t = 0.0;
            Real dt = 0.0;
            bool draws = false;
            T exp_k_dt{};        // e^{-kappa*dt}
            T sd_chi{};          // sqrt(Var[chi_{t+dt} | chi_t])
            T chol21{};          // Cov(chi, xi) / sd_chi
            T sd_xi_given_chi{}; // sqrt(Var[xi] - chol21^2 * Var[chi])
        };

        void set_param_pointers()
        {
            params_ = {&s0_, &r_, &chi0_, &kappa_, &sigma_chi_, &mu_xi_, &sigma_xi_, &rho_};
        }

        T s0_{}, r_{}, chi0_{}, kappa_{}, sigma_chi_{}, mu_xi_{}, sigma_xi_{}, rho_{};

        TimeLine timeline_;
        std::vector<SampleDef> defline_;
        std::vector<Step> steps_;
        std::size_t sim_dim_ = 0;

        std::vector<T *> params_;
    };

} // namespace quantModeling

#endif // QM_MODELS_COMMODITY_SCHWARTZ_SMITH_SIM_MODEL_HPP
