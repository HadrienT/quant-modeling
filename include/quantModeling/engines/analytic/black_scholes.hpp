#ifndef ENGINE_ANALYTIC_BS_HPP
#define ENGINE_ANALYTIC_BS_HPP

#include "quantModeling/engines/base.hpp"
#include "quantModeling/instruments/equity/vanilla.hpp"
#include "quantModeling/instruments/base.hpp"
#include "quantModeling/models/equity/black_scholes.hpp"
#include "quantModeling/utils/stats.hpp"
#include <cmath>

namespace quantModeling
{
    class BSEuroVanillaAnalyticEngine final : public EngineBase
    {
    public:
        using EngineBase::EngineBase;

        void visit(const VanillaOption &opt) override
        {
            validate(opt);
            const auto &m = require_model<IBlackScholesModel>("BSEuroVanillaAnalyticEngine");
            // const auto &m = *ctx_.model;
            const Real S0 = m.spot0();
            const Real r = m.rate_r();
            const Real q = m.yield_q();
            const Real v = m.vol_sigma();

            const Real T = opt.exercise->dates().front();
            const auto &payoff = *opt.payoff;
            const OptionType type = payoff.type();
            const Real K = payoff.strike();

            // Forward and discount factors
            const Real df_r = std::exp(-r * T);
            const Real df_q = std::exp(-q * T);
            const Real F = S0 * df_q / df_r; // forward under continuous carry

            const Real stddev = v * std::sqrt(T);
            const Real lnFK = std::log(F / K);

            const Real d1 = (lnFK + 0.5 * stddev * stddev) / stddev;
            const Real d2 = d1 - stddev;

            PricingResult out;
            out.diagnostics = "BS analytic European vanilla (flat r,q,sigma)";

            if (type == OptionType::Call)
            {
                out.npv = opt.notional * (df_r * (F * norm_cdf(d1) - K * norm_cdf(d2)));
                // Greeks (spot greeks)
                const Real Nd1 = norm_cdf(d1);
                const Real nd1 = norm_pdf(d1);

                out.greeks.delta = opt.notional * (df_q * Nd1);                    // ∂V/∂S
                out.greeks.gamma = opt.notional * (df_q * nd1 / (S0 * stddev));    // ∂²V/∂S²
                out.greeks.vega = opt.notional * (S0 * df_q * nd1 * std::sqrt(T)); // ∂V/∂σ
                // Theta and rho are included as common desk outputs; sign conventions vary.
                // Here: theta = ∂V/∂t (calendar time), negative typically; rho = ∂V/∂r.
                // Using standard continuous-carry formulas:
                out.greeks.rho = opt.notional * (T * K * df_r * norm_cdf(d2)); // ∂V/∂r
                out.greeks.theta = opt.notional * (-(S0 * df_q * nd1 * v) / (2.0 * std::sqrt(T)) - r * K * df_r * norm_cdf(d2) + q * S0 * df_q * Nd1);
            }
            else
            {
                out.npv = opt.notional * (df_r * (K * norm_cdf(-d2) - F * norm_cdf(-d1)));

                const Real Nmd1 = norm_cdf(-d1);
                const Real nd1 = norm_pdf(d1);

                out.greeks.delta = opt.notional * (df_q * (norm_cdf(d1) - 1.0)); // call delta - df_q
                out.greeks.gamma = opt.notional * (df_q * nd1 / (S0 * stddev));
                out.greeks.vega = opt.notional * (S0 * df_q * nd1 * std::sqrt(T));
                out.greeks.rho = opt.notional * (-T * K * df_r * norm_cdf(-d2));
                out.greeks.theta = opt.notional * (-(S0 * df_q * nd1 * v) / (2.0 * std::sqrt(T)) + r * K * df_r * norm_cdf(-d2) - q * S0 * df_q * Nmd1);
            }

            res_ = out;
        }

    private:
        static void validate(const VanillaOption &opt)
        {
            if (!opt.payoff)
                throw InvalidInput("VanillaOption.payoff is null");
            if (!opt.exercise)
                throw InvalidInput("VanillaOption.exercise is null");
            if (opt.exercise->type() != ExerciseType::European)
            {
                throw UnsupportedInstrument("Non-European exercise is not supported by this engine");
            }
            if (opt.exercise->dates().size() != 1)
            {
                throw InvalidInput("EuropeanExercise must contain exactly one date (maturity)");
            }
            const Real T = opt.exercise->dates().front();
            if (!(T > 0.0))
                throw InvalidInput("Maturity T must be > 0");
            if (!(opt.notional > 0.0))
                throw InvalidInput("Notional must be > 0");
            const Real K = opt.payoff->strike();
            if (!(K > 0.0))
                throw InvalidInput("Strike must be > 0");
        }
    };
}

#endif