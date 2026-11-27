#include "quantModeling/engines/analytic/curve_closed_forms.hpp"

#include "quantModeling/core/types.hpp"
#include "quantModeling/models/equity/curve_carry.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace quantModeling
{
    namespace
    {
        Real N(Real x)
        {
            return 0.5 * std::erfc(-x / std::sqrt(2.0));
        }
        Real phi(Real x)
        {
            return std::exp(-0.5 * x * x) / std::sqrt(2.0 * 3.14159265358979323846);
        }

        void require(bool ok, const char *what)
        {
            if (!ok)
                throw InvalidInput(what);
        }

        /// Black's formula undiscounted on a forward F and a total variance
        /// v: the price, ∂/∂F and ∂/∂v.
        struct Black
        {
            Real value, dF, dv;
        };

        Black black(Real F, Real K, Real v, bool is_call)
        {
            const Real w = is_call ? 1.0 : -1.0;
            if (!(v > 0.0))
                return {std::max(w * (F - K), Real(0.0)), w * (F - K) > 0.0 ? w : 0.0, 0.0};
            const Real sd = std::sqrt(v);
            const Real d1 = (std::log(F / K) + 0.5 * v) / sd, d2 = d1 - sd;
            return {w * (F * N(w * d1) - K * N(w * d2)), w * N(w * d1), F * phi(d1) / (2.0 * sd)};
        }

        void check_fixings(const std::vector<Time> &fixings, Time pay)
        {
            require(!fixings.empty(), "Asian on a curve: no fixing");
            for (std::size_t i = 0; i < fixings.size(); ++i)
                require(fixings[i] > 0.0 && (i == 0 || fixings[i] >= fixings[i - 1]),
                        "Asian on a curve: fixings must be positive and increasing");
            require(pay >= fixings.back(), "Asian on a curve: paid before its last fixing");
        }
    } // namespace

    std::vector<PillarRisk> curve_pillar_risks(const ForwardCurve &curve, const CurveSensitivity &s)
    {
        if (curve.is_flat())
            return {};
        const auto &dt = curve.discount_curve().pillar_times();
        const auto &ft = curve.pillar_times();
        std::vector<Real> zero(dt.size(), 0.0), div(ft.size(), 0.0);
        std::vector<std::pair<std::size_t, Real>> w;
        // A zero-rate shift δ_j moves ln DF(t) by −w_j(t) t_j δ_j and ln F(t)
        // by +w_j(t) t_j δ_j; a dividend-yield shift ε_i moves ln F(t) by
        // −u_i(t) t_i ε_i.
        for (const auto &[t, g] : s.log_forward)
        {
            curve.discount_curve().log_discount_weights(t, w);
            for (const auto &[j, wj] : w)
                zero[j] += g * wj * dt[j];
            curve.log_forward_weights(t, w);
            for (const auto &[i, ui] : w)
                div[i] -= g * ui * ft[i];
        }
        for (const auto &[t, h] : s.log_discount)
        {
            curve.discount_curve().log_discount_weights(t, w);
            for (const auto &[j, wj] : w)
                zero[j] -= h * wj * dt[j];
        }
        std::vector<PillarRisk> out;
        for (std::size_t j = 0; j < dt.size(); ++j)
            out.push_back({CurveCarry::rate_label(dt[j]), zero[j]});
        for (std::size_t i = 0; i < ft.size(); ++i)
            out.push_back({CurveCarry::dividend_label(ft[i]), div[i]});
        return out;
    }

    std::pair<Real, Real> curve_parallel_risks(const CurveSensitivity &s)
    {
        Real rho = 0.0, dq = 0.0;
        for (const auto &[t, g] : s.log_forward)
        {
            rho += g * t;
            dq -= g * t;
        }
        for (const auto &[t, h] : s.log_discount)
            rho -= h * t;
        return {rho, dq};
    }

    CurveClosedForm black_scholes_vanilla(const ForwardCurve &curve, Real vol, Real strike, Time T, bool is_call)
    {
        require(T > 0.0 && strike > 0.0 && vol >= 0.0, "black_scholes_vanilla: T > 0, strike > 0, vol >= 0");
        const Real F = curve.forward(T), DF = curve.discount(T);
        const Black b = black(F, strike, vol * vol * T, is_call);
        CurveClosedForm out;
        out.price = DF * b.value;
        out.sensitivity.log_forward = {{T, DF * F * b.dF}};
        out.sensitivity.log_discount = {{T, out.price}};
        return out;
    }

    CurveClosedForm black_scholes_digital(const ForwardCurve &curve, Real vol, Real strike, Time T, bool is_call,
                                          Real payout)
    {
        require(T > 0.0 && strike > 0.0 && vol > 0.0, "black_scholes_digital: T > 0, strike > 0, vol > 0");
        const Real F = curve.forward(T), DF = curve.discount(T);
        const Real sd = vol * std::sqrt(T), w = is_call ? 1.0 : -1.0;
        const Real d2 = (std::log(F / strike) - 0.5 * sd * sd) / sd;
        CurveClosedForm out;
        out.price = payout * DF * N(w * d2);
        out.sensitivity.log_forward = {{T, w * payout * DF * phi(d2) / sd}};
        out.sensitivity.log_discount = {{T, out.price}};
        return out;
    }

    CurveClosedForm equity_forward(const ForwardCurve &curve, Real strike, Time T)
    {
        require(T > 0.0, "equity_forward: T > 0");
        const Real F = curve.forward(T), DF = curve.discount(T);
        CurveClosedForm out;
        out.price = DF * (F - strike);
        out.sensitivity.log_forward = {{T, DF * F}};
        out.sensitivity.log_discount = {{T, out.price}};
        return out;
    }

    CurveClosedForm geometric_asian(const ForwardCurve &curve, Real vol, Real strike, bool is_call,
                                    const std::vector<Time> &fixings, Time pay)
    {
        check_fixings(fixings, pay);
        require(strike > 0.0 && vol >= 0.0, "geometric_asian: strike > 0, vol >= 0");
        const Real n = static_cast<Real>(fixings.size());
        Real mean_lnF = 0.0, mean_t = 0.0, sum_min = 0.0;
        for (const Time t : fixings)
        {
            mean_lnF += std::log(curve.forward(t)) / n;
            mean_t += t / n;
        }
        for (const Time ti : fixings)
            for (const Time tj : fixings)
                sum_min += std::min(ti, tj);
        const Real v = vol * vol * sum_min / (n * n);
        // E[G] = exp(E ln G + v/2), E ln G = mean ln F − σ² mean t / 2.
        const Real FG = std::exp(mean_lnF - 0.5 * vol * vol * mean_t + 0.5 * v);
        const Real DF = curve.discount(pay);
        const Black b = black(FG, strike, v, is_call);
        CurveClosedForm out;
        out.price = DF * b.value;
        for (const Time t : fixings)
            out.sensitivity.log_forward.emplace_back(t, DF * FG * b.dF / n);
        out.sensitivity.log_discount = {{pay, out.price}};
        return out;
    }

    CurveClosedForm arithmetic_asian_levy(const ForwardCurve &curve, Real vol, Real strike, bool is_call,
                                          const std::vector<Time> &fixings, Time pay)
    {
        check_fixings(fixings, pay);
        require(strike > 0.0 && vol >= 0.0, "arithmetic_asian_levy: strike > 0, vol >= 0");
        const std::size_t n = fixings.size();
        const Real nn = static_cast<Real>(n);
        std::vector<Real> F(n), row(n, 0.0);
        Real M1 = 0.0, M2 = 0.0;
        for (std::size_t i = 0; i < n; ++i)
        {
            F[i] = curve.forward(fixings[i]);
            M1 += F[i] / nn;
        }
        // row_i = Σ_j F_j e^{σ² min(t_i, t_j)}: M2 = Σ_i F_i row_i / n².
        for (std::size_t i = 0; i < n; ++i)
        {
            for (std::size_t j = 0; j < n; ++j)
                row[i] += F[j] * std::exp(vol * vol * std::min(fixings[i], fixings[j]));
            M2 += F[i] * row[i] / (nn * nn);
        }
        // The lognormal of mean M1 and second moment M2: total variance
        // v = ln(M2 / M1²).
        const Real v = std::max(std::log(M2 / (M1 * M1)), Real(0.0));
        const Real DF = curve.discount(pay);
        const Black b = black(M1, strike, v, is_call);
        CurveClosedForm out;
        out.price = DF * b.value;
        for (std::size_t i = 0; i < n; ++i)
        {
            // ∂M1/∂ln F_i = F_i/n; ∂ln M2/∂ln F_i = 2 F_i row_i / (n² M2).
            const Real dM1 = F[i] / nn;
            const Real dlnM2 = 2.0 * F[i] * row[i] / (nn * nn * M2);
            const Real dv = v > 0.0 ? dlnM2 - 2.0 * dM1 / M1 : 0.0;
            out.sensitivity.log_forward.emplace_back(fixings[i], DF * (b.dF * dM1 + b.dv * dv));
        }
        out.sensitivity.log_discount = {{pay, out.price}};
        return out;
    }

} // namespace quantModeling
