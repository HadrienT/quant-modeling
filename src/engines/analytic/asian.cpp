#include "quantModeling/engines/analytic/asian.hpp"
#include "quantModeling/engines/base.hpp"
#include "quantModeling/instruments/base.hpp"
#include "quantModeling/instruments/equity/asian.hpp"
#include "quantModeling/models/equity/black_scholes.hpp"
#include "quantModeling/utils/stats.hpp"
#include <cmath>
#include <stdexcept>
#include <iostream>

namespace quantModeling
{

    void BSEuroArithmeticAsianAnalyticEngine::visit(const AsianOption &opt)
    {
        validate(opt);
        const auto &m = require_model<IBlackScholesModel>("BSEuroArithmeticAsianAnalyticEngine");

        const Real S0 = m.spot0();
        const Real r = m.rate_r();
        const Real q = m.yield_q();
        const Real sigma = m.vol_sigma();

        const Real T = opt.exercise->dates().front();
        const auto &payoff = *opt.payoff;
        const OptionType type = payoff.type();
        const Real K = payoff.strike();

        PricingResult out;
        out.diagnostics = "BS Turnbull-Wakeman approx for arithmetic Asian (flat r,q,sigma)";

        // Basic validation to prevent NaNs
        if (!(S0 > 0.0) || !(K > 0.0) || !(T > 0.0))
        {
            // Degenerate: maturity now or invalid inputs
            const Real intrinsic = (type == OptionType::Call) ? std::max(S0 - K, 0.0) : std::max(K - S0, 0.0);
            out.npv = opt.notional * intrinsic; // no discount if T<=0; adjust if your convention differs
            res_ = out;
            return;
        }

        const Real df_r = std::exp(-r * T);
        // const Real df_q = std::exp(-q * T);

        // Forward of the arithmetic average (continuous monitoring): E[A_T]
        const Real mu = r - q;
        Real F_A;
        if (std::abs(mu) < 1e-12)
        {
            F_A = S0; // limit mu->0
        }
        else
        {
            F_A = S0 * (std::exp(mu * T) - 1.0) / (mu * T);
        }

        // Handle near-zero vol: average is (almost) deterministic
        if (sigma <= 0.0)
        {
            const Real intrinsic = (type == OptionType::Call) ? std::max(F_A - K, 0.0) : std::max(K - F_A, 0.0);
            out.npv = opt.notional * df_r * intrinsic;
            res_ = out;
            return;
        }

        // Compute second moment E[A^2] and M = E[A^2] / E[A]^2 robustly
        const Real alpha = mu;           // r - q
        const Real beta = sigma * sigma; // sigma^2

        const Real B = 2.0 * alpha + beta;

        // t1 = (e^{B T} - 1)/B, t2 = (e^{alpha T} - 1)/alpha
        const Real t1 = (std::abs(B) < 1e-16) ? T : (std::expm1(B * T) / B);
        const Real t2 = (std::abs(alpha) < 1e-16) ? T : (std::expm1(alpha * T) / alpha);

        const Real EA2 = (2.0 * S0 * S0 / (T * T * (alpha + beta))) * (t1 - t2);
        const Real EA = F_A; // previously computed E[A]

        if (!(EA > 0.0) || !(EA2 > 0.0))
        {
            const Real intrinsic = (type == OptionType::Call) ? std::max(F_A - K, 0.0) : std::max(K - F_A, 0.0);
            out.npv = opt.notional * df_r * intrinsic;
            res_ = out;
            return;
        }

        const Real M = EA2 / (EA * EA);
        const Real logM = std::log(M);
        Real stddev_A;

        // Debug: print Turnbull-Wakeman / moment-matching terms and derived quantities

        // For lognormal matching: Var(log A) = logM, so stddev = sqrt(logM)
        // If logM <= 0 (can happen due to numerical error), clamp to 0
        if (logM <= 0.0)
        {
            stddev_A = 0.0;
        }
        else
        {
            stddev_A = std::sqrt(logM);
        }

        // If stddev_A == 0 -> deterministic
        if (stddev_A <= 1e-14)
        {
            const Real intrinsic = (type == OptionType::Call) ? std::max(F_A - K, 0.0) : std::max(K - F_A, 0.0);
            out.npv = opt.notional * df_r * intrinsic;
            res_ = out;
            return;
        }

        const Real lnFK = std::log(F_A / K);
        const Real d1 = (lnFK + 0.5 * stddev_A * stddev_A) / stddev_A;
        const Real d2 = d1 - stddev_A;

        if (type == OptionType::Call)
        {
            out.npv = opt.notional * (df_r * (F_A * norm_cdf(d1) - K * norm_cdf(d2)));
        }
        else
        {
            out.npv = opt.notional * (df_r * (K * norm_cdf(-d2) - F_A * norm_cdf(-d1)));
        }

        // Greeks: analytic delta/gamma (chain rule) and finite-difference vega/rho/theta
        const Real Nd1 = norm_cdf(d1);
        // const Real Nd2 = norm_cdf(d2);
        const Real nd1 = norm_pdf(d1);

        // dF_A/dS0 for the chosen forward formula (F_A = S0 * g(mu,T))
        const Real dF_dS = F_A / S0;

        // Total log-volatility used in Black d1 (stddev over horizon)
        const Real sigma_tot = stddev_A; // already represents total stddev used in d1

        // Delta: dPrice/dS0 = dPrice/dF_A * dF_A/dS0 ; dPrice/dF_A (Black) = df_r * N(d1)
        out.greeks.delta = opt.notional * (df_r * Nd1 * dF_dS);

        // Gamma: derived via chain rule. da/dS0 == 0 since F_A = S0 * g(mu,T)
        if (sigma_tot > 0.0)
        {
            out.greeks.gamma = opt.notional * (df_r * nd1 * (dF_dS / (S0 * sigma_tot)));
        }
        else
        {
            out.greeks.gamma = 0.0;
        }

        // Helper to compute price for central finite-differences (used for vega/rho/theta)
        auto price_with_params = [&](Real r_p, Real sigma_p, Real T_p) -> Real
        {
            // forward for arithmetic average (same formula, but with sigma/r/T replaced)
            const Real mu_p = r_p - q;
            Real Fp;
            if (std::abs(mu_p) < 1e-12)
                Fp = S0;
            else
                Fp = S0 * (std::exp(mu_p * T_p) - 1.0) / (mu_p * T_p);

            if (!(Fp > 0.0))
                return opt.notional * df_r * std::max((type == OptionType::Call) ? (Fp - K) : (K - Fp), 0.0);

            if (sigma_p <= 0.0)
            {
                const Real intrinsic = (type == OptionType::Call) ? std::max(Fp - K, 0.0) : std::max(K - Fp, 0.0);
                return opt.notional * std::exp(-r_p * T_p) * intrinsic;
            }

            const Real x_p = (sigma_p * sigma_p) * T_p;
            const Real y_p = 0.5 * x_p;
            const Real a_p = std::expm1(y_p);
            const Real term_p = a_p - y_p;
            Real M_p;
            if (std::abs(x_p) < 1e-8)
                M_p = 0.25;
            else
                M_p = (2.0 * std::exp(y_p) * term_p) / (x_p * x_p);

            if (!(M_p > 0.0))
            {
                const Real intrinsic = (type == OptionType::Call) ? std::max(Fp - K, 0.0) : std::max(K - Fp, 0.0);
                return opt.notional * std::exp(-r_p * T_p) * intrinsic;
            }

            const Real logM_p = std::log(M_p);
            Real sigma_tot_p = 0.0;
            if (logM_p > 0.0)
                sigma_tot_p = std::sqrt(logM_p);
            else
                sigma_tot_p = 0.0;

            if (sigma_tot_p <= 1e-14)
            {
                const Real intrinsic = (type == OptionType::Call) ? std::max(Fp - K, 0.0) : std::max(K - Fp, 0.0);
                return opt.notional * std::exp(-r_p * T_p) * intrinsic;
            }

            const Real lnFK_p = std::log(Fp / K);
            const Real d1p = (lnFK_p + 0.5 * sigma_tot_p * sigma_tot_p) / sigma_tot_p;
            const Real d2p = d1p - sigma_tot_p;

            if (type == OptionType::Call)
                return opt.notional * std::exp(-r_p * T_p) * (Fp * norm_cdf(d1p) - K * norm_cdf(d2p));
            else
                return opt.notional * std::exp(-r_p * T_p) * (K * norm_cdf(-d2p) - Fp * norm_cdf(-d1p));
        };

        // Vega: derivative w.r.t model volatility (annual sigma)
        const Real eps_sigma = std::max(1e-6, sigma * 1e-3);
        const Real price_sp = price_with_params(r, sigma + eps_sigma, T);
        const Real price_sm = price_with_params(r, sigma - eps_sigma, T);
        out.greeks.vega = (price_sp - price_sm) / (2.0 * eps_sigma);

        // Rho: derivative w.r.t r
        const Real eps_r = std::max(1e-6, std::abs(r) * 1e-3);
        const Real price_rp = price_with_params(r + eps_r, sigma, T);
        const Real price_rm = price_with_params(r - eps_r, sigma, T);
        out.greeks.rho = (price_rp - price_rm) / (2.0 * eps_r);

        // Theta: approximate change when one day elapses (T decreases)
        const Real eps_T = 1.0 / 365.0; // one day in years
        const Real T_minus = std::max(1e-8, T - eps_T);
        const Real price_Tm = price_with_params(r, sigma, T_minus);
        const Real price_Tp = price_with_params(r, sigma, T + eps_T);
        // Theta per year (price change when time passes -> T decreases): (price(T - dt)-price(T + dt)) / (2 dt)
        out.greeks.theta = (price_Tm - price_Tp) / (2.0 * eps_T);

        res_ = out;
    }

    void BSEuroGeometricAsianAnalyticEngine::visit(const AsianOption &opt)
    {
        validate(opt);
        const auto &m = require_model<IBlackScholesModel>("BSEuroGeometricAsianAnalyticEngine");

        const Real S0 = m.spot0();
        const Real r = m.rate_r();
        const Real q = m.yield_q();
        const Real sigma = m.vol_sigma();

        const Real T = opt.exercise->dates().front();
        const auto &payoff = *opt.payoff;
        const OptionType type = payoff.type();
        const Real K = payoff.strike();

        // For geometric average, the effective volatility is reduced
        const Real sigma_G = sigma / std::sqrt(3.0);

        // Effective drift adjustment
        const Real b = r - q; // cost of carry
        const Real b_G = (b - 0.5 * sigma * sigma) / 2.0 + 0.5 * sigma_G * sigma_G;

        const Real df_r = std::exp(-r * T);
        const Real df_q = std::exp(-q * T);
        const Real F = S0 * std::exp(b_G * T);

        const Real stddev_G = sigma_G * std::sqrt(T);
        const Real lnFK = std::log(F / K);

        const Real d1 = (lnFK + 0.5 * stddev_G * stddev_G) / stddev_G;
        const Real d2 = d1 - stddev_G;

        PricingResult out;
        out.diagnostics = "BS closed-form solution for geometric Asian (flat r,q,sigma)";

        if (type == OptionType::Call)
        {
            out.npv = opt.notional * (df_r * (F * norm_cdf(d1) - K * norm_cdf(d2)));

            const Real Nd1 = norm_cdf(d1);
            const Real nd1 = norm_pdf(d1);

            out.greeks.delta = opt.notional * (df_q * Nd1);
            out.greeks.gamma = opt.notional * (df_q * nd1 / (S0 * stddev_G));
            out.greeks.vega = opt.notional * (S0 * df_q * nd1 * T / 3.0);
            out.greeks.rho = opt.notional * (T * K * df_r * norm_cdf(d2));
            out.greeks.theta = opt.notional * (-(S0 * df_q * nd1 * sigma_G) / (2.0 * std::sqrt(T)) -
                                               r * K * df_r * norm_cdf(d2) + q * S0 * df_q * Nd1);
        }
        else
        {
            out.npv = opt.notional * (df_r * (K * norm_cdf(-d2) - F * norm_cdf(-d1)));

            const Real Nmd1 = norm_cdf(-d1);
            const Real nd1 = norm_pdf(d1);

            out.greeks.delta = opt.notional * (df_q * (norm_cdf(d1) - 1.0));
            out.greeks.gamma = opt.notional * (df_q * nd1 / (S0 * stddev_G));
            out.greeks.vega = opt.notional * (S0 * df_q * nd1 * T / 3.0);
            out.greeks.rho = opt.notional * (-T * K * df_r * norm_cdf(-d2));
            out.greeks.theta = opt.notional * (-(S0 * df_q * nd1 * sigma_G) / (2.0 * std::sqrt(T)) +
                                               r * K * df_r * norm_cdf(-d2) - q * S0 * df_q * Nmd1);
        }

        res_ = out;
    }

    void BSEuroArithmeticAsianAnalyticEngine::validate(const AsianOption &opt)
    {
        if (!opt.payoff)
            throw InvalidInput("AsianOption.payoff is null");
        if (!opt.exercise)
            throw InvalidInput("AsianOption.exercise is null");
        if (opt.exercise->type() != ExerciseType::European)
            throw UnsupportedInstrument("Non-European exercise is not supported by this engine");
        if (opt.exercise->dates().size() != 1)
            throw InvalidInput("Expected single maturity date for European Asian option");
    }

    void BSEuroGeometricAsianAnalyticEngine::validate(const AsianOption &opt)
    {
        if (!opt.payoff)
            throw InvalidInput("AsianOption.payoff is null");
        if (!opt.exercise)
            throw InvalidInput("AsianOption.exercise is null");
        if (opt.exercise->type() != ExerciseType::European)
            throw UnsupportedInstrument("Non-European exercise is not supported by this engine");
        if (opt.exercise->dates().size() != 1)
            throw InvalidInput("Expected single maturity date for European Asian option");
    }

} // namespace quantModeling
