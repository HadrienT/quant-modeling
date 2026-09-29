#ifndef QM_MODELS_HYBRID_HULL_WHITE_EQUITY_SIM_MODEL_HPP
#define QM_MODELS_HYBRID_HULL_WHITE_EQUITY_SIM_MODEL_HPP

#include "quantModeling/aad/number.hpp"
#include "quantModeling/core/types.hpp"
#include "quantModeling/models/rates/hull_white_curve.hpp"
#include "quantModeling/models/simulation_model.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace quantModeling
{

    namespace detail
    {
        /// ∫_a^b f by 20-point Gauss-Legendre: exact to rounding for the
        /// smooth exponentials the Hull-White step moments are made of.
        template <class F>
        double gauss_legendre(F f, double a, double b)
        {
            static constexpr std::array<double, 10> x{
                0.0765265211334973, 0.2277858511416451, 0.3737060887154195, 0.5108670019508271,
                0.6360536807265150, 0.7463319064601508, 0.8391169718222188, 0.9122344282513259,
                0.9639719272779138, 0.9931285991850949};
            static constexpr std::array<double, 10> w{
                0.1527533871307258, 0.1491729864726037, 0.1420961093183820, 0.1316886384491766,
                0.1181945319615184, 0.1019301198172404, 0.0832767415767048, 0.0626720483341091,
                0.0406014298003869, 0.0176140071391521};
            const double m = 0.5 * (a + b), h = 0.5 * (b - a);
            double s = 0.0;
            for (std::size_t i = 0; i < x.size(); ++i)
                s += w[i] * (f(m - h * x[i]) + f(m + h * x[i]));
            return h * s;
        }
    } // namespace detail

    /**
     * @brief An equity under stochastic Hull-White rates, for scripts
     *        (issue #86: equity-rates hybrids; blueprint/wp/21-rates.md §6).
     *
     *   r(t) = f(0, t) + x(t),  dx = (y(t) − a x) dt + σ_r dW_r   (HullWhiteCurveModel)
     *   dS / S = (r − q) dt + σ_S dW_S,  d⟨W_r, W_S⟩ = ρ dt
     *
     * The numeraire is the bank account B(t) = exp(∫ r) = exp(∫ x) / P(0, t).
     * Between two event dates, (x(t), ∫ x, W_S increment) is Gaussian given
     * x(s): the step draws three normals and applies the exact 3 × 3
     * covariance (moments by Gauss-Legendre at init), so the simulation has
     * no discretisation bias and needs no step between event dates.
     * log S(t) = log S0 − log P(0, t) + ∫ x − (q + σ_S²/2) t + σ_S W_S(t):
     * S / B is S0 e^{−qt} times an exponential martingale, today's curve is
     * repriced exactly, and df(T) in a script reads P(t, T | x(t)).
     *
     * A script that ignores spot() is a pure rates product: a Bermudan
     * swaption written with df() and exercise() (tests/testHybrid.cpp checks
     * it against the Hull-White lattice).
     *
     * Differentiable parameters: spot, dividend yield, equity vol. The rates
     * side (curve, a, σ_r, ρ) is double.
     */
    template <class T = Real>
    class HullWhiteEquitySimModel final : public ISimulationModel<T>
    {
      public:
        HullWhiteEquitySimModel(T s0, T q, T sigma_s, HullWhiteCurveModel rates, Real rho)
            : s0_(s0), q_(q), sigma_s_(sigma_s), rates_(std::move(rates)), rho_(rho)
        {
            if (!(rho_ >= -1.0 && rho_ <= 1.0))
                throw InvalidInput("hull_white: the equity-rates correlation must lie in [-1, 1]");
            set_param_pointers();
        }

        HullWhiteEquitySimModel(const HullWhiteEquitySimModel &o)
            : s0_(o.s0_), q_(o.q_), sigma_s_(o.sigma_s_), rates_(o.rates_), rho_(o.rho_), timeline_(o.timeline_), defline_(o.defline_), steps_(o.steps_), sim_dim_(o.sim_dim_)
        {
            set_param_pointers();
        }

        const HullWhiteCurveModel &rates() const { return rates_; }

        std::size_t n_underlyings() const override { return 1; }

        void init(const TimeLine &product_timeline, const std::vector<SampleDef> &defline) override
        {
            timeline_ = canonical_timeline(product_timeline);
            defline_ = defline;
            steps_.clear();
            sim_dim_ = 0;
            const Real a = rates_.mean_reversion(), sr = rates_.sigma();
            Time s = 0.0;
            for (const Time t : timeline_)
            {
                Step st;
                st.t = t;
                st.draws = t > TIMELINE_EPS;
                if (st.draws)
                {
                    const auto G = [&](double v)
                    { return rates_.G(v, t); };
                    const auto E = [&](double v)
                    { return std::exp(-a * (t - v)); };
                    st.decay = E(s);
                    st.G_step = rates_.G(s, t);
                    st.mean_x = detail::gauss_legendre([&](double v)
                                                       { return E(v) * rates_.y(v); }, s, t);
                    st.mean_I = detail::gauss_legendre([&](double v)
                                                       { return rates_.y(v) * G(v); }, s, t);
                    // Covariance of (x(t) − E, ∫x − E, W_S(t) − W_S(s)).
                    double C[3][3];
                    C[0][0] = sr * sr * detail::gauss_legendre([&](double v)
                                                               { return E(v) * E(v); }, s, t);
                    C[1][1] = sr * sr * detail::gauss_legendre([&](double v)
                                                               { return G(v) * G(v); }, s, t);
                    C[0][1] = C[1][0] = sr * sr * detail::gauss_legendre([&](double v)
                                                                         { return E(v) * G(v); }, s, t);
                    C[0][2] = C[2][0] = rho_ * sr * st.G_step;
                    C[1][2] = C[2][1] = rho_ * sr * detail::gauss_legendre(G, s, t);
                    C[2][2] = t - s;
                    cholesky(C, st.L);
                    st.dt = t - s;
                    st.log_df_ratio = std::log(rates_.discount().discount(s) / rates_.discount().discount(t));
                    sim_dim_ += 3;
                }
                steps_.push_back(st);
                s = t;
            }
        }

        const TimeLine &sim_timeline() const override { return timeline_; }
        std::size_t sim_dim() const override { return sim_dim_; }

        void generate_path(std::span<const double> gaussians, Scenario<T> &path) const override
        {
            using std::exp;
            double x = 0.0, I = 0.0;
            T log_s = T(0.0);
            std::size_t g = 0;
            for (std::size_t i = 0; i < steps_.size(); ++i)
            {
                const Step &st = steps_[i];
                if (st.draws)
                {
                    const double z0 = gaussians[g], z1 = gaussians[g + 1], z2 = gaussians[g + 2];
                    g += 3;
                    const double e0 = st.L[0][0] * z0;
                    const double e1 = st.L[1][0] * z0 + st.L[1][1] * z1;
                    const double w = st.L[2][0] * z0 + st.L[2][1] * z1 + st.L[2][2] * z2;
                    const double I_step = x * st.G_step + st.mean_I + e1;
                    x = x * st.decay + st.mean_x + e0;
                    I += I_step;
                    log_s = log_s + (st.log_df_ratio + I_step) - (q_ + 0.5 * sigma_s_ * sigma_s_) * st.dt +
                            sigma_s_ * w;
                }
                const T S = s0_ * exp(log_s);
                Sample<T> &smp = path[i];
                smp.spots.assign(1, S);
                smp.numeraire = T(std::exp(I) / rates_.discount().discount(st.t));
                const SampleDef &def = defline_[i];
                smp.discounts.resize(def.discount_mats.size());
                for (std::size_t k = 0; k < def.discount_mats.size(); ++k)
                    smp.discounts[k] = T(rates_.zcb(st.t, def.discount_mats[k], x));
                smp.forwards.resize(def.forward_mats.size());
                for (std::size_t k = 0; k < def.forward_mats.size(); ++k)
                {
                    const Time mat = def.forward_mats[k];
                    smp.forwards[k] = S * exp(-q_ * (mat - st.t)) / rates_.zcb(st.t, mat, x);
                }
            }
        }

        std::unique_ptr<ISimulationModel<T>> clone() const override
        {
            return std::make_unique<HullWhiteEquitySimModel<T>>(*this);
        }

        /// S / B = S0 e^{−qt} × an exponential martingale.
        bool deflated_spot_mean(std::size_t, Time t, Real &mean) const override
        {
            mean = to_double(s0_) * std::exp(-to_double(q_) * t);
            return true;
        }

        const std::vector<T *> &parameters() const override { return params_; }
        const std::vector<std::string> &parameter_labels() const override
        {
            static const std::vector<std::string> labels{"spot", "div", "vol"};
            return labels;
        }

      private:
        struct Step
        {
            Time t = 0.0;
            bool draws = false;
            double dt = 0.0, decay = 1.0, G_step = 0.0, mean_x = 0.0, mean_I = 0.0, log_df_ratio = 0.0;
            double L[3][3] = {};
        };

        /// Lower Cholesky factor of a 3 × 3 covariance, a zero column where a
        /// pivot vanishes (|ρ| = 1, or no equity leg): semidefinite is fine.
        static void cholesky(const double C[3][3], double L[3][3])
        {
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j <= i; ++j)
                {
                    double s = C[i][j];
                    for (int k = 0; k < j; ++k)
                        s -= L[i][k] * L[j][k];
                    if (i == j)
                        L[i][i] = s > 1e-300 ? std::sqrt(s) : 0.0;
                    else
                        L[i][j] = L[j][j] > 0.0 ? s / L[j][j] : 0.0;
                }
        }

        void set_param_pointers() { params_ = {&s0_, &q_, &sigma_s_}; }

        T s0_{}, q_{}, sigma_s_{};
        HullWhiteCurveModel rates_;
        Real rho_ = 0.0;
        TimeLine timeline_;
        std::vector<SampleDef> defline_;
        std::vector<Step> steps_;
        std::size_t sim_dim_ = 0;
        std::vector<T *> params_;
    };

} // namespace quantModeling

#endif // QM_MODELS_HYBRID_HULL_WHITE_EQUITY_SIM_MODEL_HPP
