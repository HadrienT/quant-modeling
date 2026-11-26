#ifndef QM_MODELS_HYBRID_HULL_WHITE_EQUITY_SIM_MODEL_HPP
#define QM_MODELS_HYBRID_HULL_WHITE_EQUITY_SIM_MODEL_HPP

#include "quantModeling/aad/number.hpp"
#include "quantModeling/core/types.hpp"
#include "quantModeling/models/equity/last_step.hpp"
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
     * @brief The directions in which the curve of a Hull-White hybrid is
     *        differentiated: the pillar zero rates move as
     *        z_j += Σ_k basis[j · K + k] θ_k (row-major, pillars × factors),
     *        and each θ_k, at 0, is a model parameter.
     *
     * The basis is the caller's: the identity gives the risks to the zero
     * rates; the bootstrap's Jacobian dz/dq gives those to the par quotes
     * the curve was built from (a bucketed DV01), each with a Monte-Carlo
     * error of its own -- a map applied after the run would need the
     * covariance of the risks, which the engine does not report. Empty: one
     * factor, "curve_parallel", moving every zero rate (or the flat rate)
     * by the same amount.
     */
    struct CurveFactors
    {
        std::vector<double> basis;
        std::vector<std::string> labels;
    };

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
     * Differentiable parameters: spot, dividend yield, equity vol, σ_r
     * ("rate_vol") and the curve's factors (CurveFactors), the mean
     * reversion and ρ held. With a fixed, the step's moments scale exactly:
     * the drifts of x and ∫x and y(t) as σ_r², the rates rows of the
     * Cholesky factor as σ_r, the equity row not at all -- so they are
     * computed once at σ_r = 1 and scaled on the path. The curve enters only
     * through log P(0, ·) at known dates, linear in the pillars' log
     * discount factors (DiscountCurve::log_discount_weights): log P(0, t) =
     * c(t) − Σ_k θ_k g_k(t), g_k tabulated at init.
     */
    template <class T = Real>
    class HullWhiteEquitySimModel final : public ISimulationModel<T>
    {
      public:
        HullWhiteEquitySimModel(T s0, T q, T sigma_s, HullWhiteCurveModel rates, Real rho,
                                CurveFactors factors = {})
            : s0_(s0), q_(q), sigma_s_(sigma_s), sigma_r_(T(rates.sigma())), rates_(std::move(rates)), rho_(rho)
        {
            if (!(rho_ >= -1.0 && rho_ <= 1.0))
                throw InvalidInput("hull_white: the equity-rates correlation must lie in [-1, 1]");
            const std::size_t pillars = rates_.discount().pillar_times().size();
            if (factors.labels.empty())
            {
                factors.labels = {"curve_parallel"};
                factors.basis.assign(pillars, 1.0);
            }
            else if (factors.basis.size() != pillars * factors.labels.size() || pillars == 0)
                throw InvalidInput("hull_white: the curve factors need one row per pillar and one column per label");
            factors_ = std::move(factors);
            theta_.assign(factors_.labels.size(), T(0.0));
            labels_ = {"spot", "div", "vol", "rate_vol"};
            labels_.insert(labels_.end(), factors_.labels.begin(), factors_.labels.end());
            set_param_pointers();
        }

        HullWhiteEquitySimModel(const HullWhiteEquitySimModel &o)
            : s0_(o.s0_), q_(o.q_), sigma_s_(o.sigma_s_), sigma_r_(o.sigma_r_), rates_(o.rates_), rho_(o.rho_), factors_(o.factors_), theta_(o.theta_), labels_(o.labels_), timeline_(o.timeline_), defline_(o.defline_), steps_(o.steps_), sim_dim_(o.sim_dim_)
        {
            set_param_pointers();
        }

        const HullWhiteCurveModel &rates() const { return rates_; }

        std::size_t n_underlyings() const override { return 1; }

        void init(const TimeLine &product_timeline, const std::vector<SampleDef> &defline) override
        {
            refuse_bridge_survivals(defline, "HullWhiteEquitySimModel"); // lastStep*: generate_path
            refuse_knock_outs(defline, "HullWhiteEquitySimModel");
            timeline_ = canonical_timeline(product_timeline);
            defline_ = defline;
            steps_.clear();
            sim_dim_ = 0;
            const Real a = rates_.mean_reversion();
            // y(v) at σ_r = 1
            const auto y_unit = [a](double v)
            { return (1.0 - std::exp(-2.0 * a * v)) / (2.0 * a); };
            Time s = 0.0;
            for (const Time t : timeline_)
            {
                Step st;
                st.t = t;
                st.at = point(t);
                st.y_unit = y_unit(t);
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
                                                       { return E(v) * y_unit(v); }, s, t);
                    st.mean_I = detail::gauss_legendre([&](double v)
                                                       { return y_unit(v) * G(v); }, s, t);
                    // Covariance of (x(t) − E, ∫x − E, W_S(t) − W_S(s)) at σ_r = 1.
                    double C[3][3];
                    C[0][0] = detail::gauss_legendre([&](double v)
                                                     { return E(v) * E(v); }, s, t);
                    C[1][1] = detail::gauss_legendre([&](double v)
                                                     { return G(v) * G(v); }, s, t);
                    C[0][1] = C[1][0] = detail::gauss_legendre([&](double v)
                                                               { return E(v) * G(v); }, s, t);
                    C[0][2] = C[2][0] = rho_ * st.G_step;
                    C[1][2] = C[2][1] = rho_ * detail::gauss_legendre(G, s, t);
                    C[2][2] = t - s;
                    cholesky(C, st.L);
                    st.dt = t - s;
                    sim_dim_ += 3;
                }
                for (const Time m : defline[steps_.size()].discount_mats)
                    st.discounts.push_back({point(m), rates_.G(t, m)});
                for (const Time m : defline[steps_.size()].forward_mats)
                    st.forwards.push_back({point(m), rates_.G(t, m)});
                steps_.push_back(std::move(st));
                s = t;
            }
        }

        const TimeLine &sim_timeline() const override { return timeline_; }
        std::size_t sim_dim() const override { return sim_dim_; }

        void generate_path(std::span<const double> gaussians, Scenario<T> &path) const override
        {
            using std::exp;
            const T sr = sigma_r_, sr2 = sigma_r_ * sigma_r_;
            T x(0.0), I(0.0), log_s(0.0), log_p_prev(0.0);
            std::size_t g = 0;
            for (std::size_t i = 0; i < steps_.size(); ++i)
            {
                const Step &st = steps_[i];
                const T log_p = log_discount(st.at);
                if (st.draws)
                {
                    const double z0 = gaussians[g], z1 = gaussians[g + 1], z2 = gaussians[g + 2];
                    g += 3;
                    const T e0 = sr * (st.L[0][0] * z0);
                    const T e1 = sr * (st.L[1][0] * z0 + st.L[1][1] * z1);
                    const double w = st.L[2][0] * z0 + st.L[2][1] * z1 + st.L[2][2] * z2;
                    const T I_step = x * st.G_step + sr2 * st.mean_I + e1;
                    x = x * st.decay + sr2 * st.mean_x + e0;
                    I = I + I_step;
                    log_s = log_s + (log_p_prev - log_p + I_step) - (q_ + 0.5 * sigma_s_ * sigma_s_) * st.dt +
                            sigma_s_ * w;
                }
                log_p_prev = log_p;
                const T S = s0_ * exp(log_s);
                Sample<T> &smp = path[i];
                smp.spots.assign(1, S);
                smp.numeraire = exp(I - log_p);
                // lastStep*() (WP 27 §8 rung 1): this model has no closed form
                // over its last step, so the slot is the payoff of the simulated
                // spot -- an unbiased estimate of that expectation, the hard
                // payoff the primitive replaces (issue #363: the library's
                // digital reads lastStepDigital under every model).
                if (!defline_[i].monitors.empty())
                    mc::fill_last_step(defline_[i], smp, S, T(0.0));
                const T half_y = 0.5 * sr2 * st.y_unit;
                // P(t, m | x) = P(0, m) / P(0, t) exp(−G x − ½ G² y(t))
                const auto zcb = [&](const Maturity &m)
                { return exp(log_discount(m.at) - log_p - m.G * x - m.G * m.G * half_y); };
                smp.discounts.resize(st.discounts.size());
                for (std::size_t k = 0; k < st.discounts.size(); ++k)
                    smp.discounts[k] = zcb(st.discounts[k]);
                smp.forwards.resize(st.forwards.size());
                for (std::size_t k = 0; k < st.forwards.size(); ++k)
                {
                    const Time mat = defline_[i].forward_mats[k];
                    smp.forwards[k] = S * exp(-q_ * (mat - st.t)) / zcb(st.forwards[k]);
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
        const std::vector<std::string> &parameter_labels() const override { return labels_; }

      private:
        /// log P(0, t) = c − Σ_k θ_k g_k.
        struct Point
        {
            double c = 0.0;
            std::vector<double> g;
        };
        struct Maturity
        {
            Point at;
            double G = 0.0; ///< G(t, maturity)
        };
        struct Step
        {
            Time t = 0.0;
            bool draws = false;
            double dt = 0.0, decay = 1.0, G_step = 0.0, mean_x = 0.0, mean_I = 0.0, y_unit = 0.0;
            double L[3][3] = {};
            Point at;
            std::vector<Maturity> discounts, forwards;
        };

        Point point(Time t) const
        {
            const DiscountCurve &curve = rates_.discount();
            Point p;
            p.c = t > 0.0 ? std::log(curve.discount(t)) : 0.0;
            const std::size_t K = theta_.size();
            p.g.assign(K, 0.0);
            if (t <= 0.0)
                return p;
            const std::vector<Time> &pillars = curve.pillar_times();
            if (pillars.empty()) // a flat rate: the parallel factor moves it
            {
                p.g[0] = t;
                return p;
            }
            // log DF_j = −z_j t_j, so ∂ log P(0, t) / ∂ z_j = −w_j t_j
            std::vector<std::pair<std::size_t, Real>> w;
            curve.log_discount_weights(t, w);
            for (const auto &[j, wj] : w)
                for (std::size_t k = 0; k < K; ++k)
                    p.g[k] += wj * pillars[j] * factors_.basis[j * K + k];
            return p;
        }

        T log_discount(const Point &p) const
        {
            T v(p.c);
            for (std::size_t k = 0; k < p.g.size(); ++k)
                if (p.g[k] != 0.0)
                    v = v - theta_[k] * p.g[k];
            return v;
        }

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

        void set_param_pointers()
        {
            params_ = {&s0_, &q_, &sigma_s_, &sigma_r_};
            for (T &t : theta_)
                params_.push_back(&t);
        }

        T s0_{}, q_{}, sigma_s_{}, sigma_r_{};
        HullWhiteCurveModel rates_;
        Real rho_ = 0.0;
        CurveFactors factors_;
        std::vector<T> theta_;
        std::vector<std::string> labels_;
        TimeLine timeline_;
        std::vector<SampleDef> defline_;
        std::vector<Step> steps_;
        std::size_t sim_dim_ = 0;
        std::vector<T *> params_;
    };

} // namespace quantModeling

#endif // QM_MODELS_HYBRID_HULL_WHITE_EQUITY_SIM_MODEL_HPP
