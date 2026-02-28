#include "quantModeling/engines/analytic/digital.hpp"
#include "quantModeling/core/types.hpp"
#include "quantModeling/models/equity/local_vol_model.hpp"
#include "quantModeling/utils/stats.hpp"
#include <cmath>
#include <string>

namespace quantModeling
{

    void BSDigitalAnalyticEngine::validate(const DigitalOption &opt)
    {
        if (!opt.payoff)
            throw InvalidInput("DigitalOption: payoff is null");
        if (!opt.exercise || opt.exercise->dates().empty())
            throw InvalidInput("DigitalOption: exercise is null or has no dates");
        if (opt.notional == 0.0)
            throw InvalidInput("DigitalOption: notional must be non-zero");
        if (opt.payoff_type == DigitalPayoffType::CashOrNothing && opt.cash_amount < 0.0)
            throw InvalidInput("DigitalOption: cash_amount must be >= 0");
    }

    void BSDigitalAnalyticEngine::visit(const DigitalOption &opt)
    {
        validate(opt);
        const auto &m = require_model<ILocalVolModel>("BSDigitalAnalyticEngine");

        const Real S = m.spot0();
        const Real r = m.rate_r();
        const Real q = m.yield_q();
        const Real v = m.vol_sigma();
        const Real T = opt.exercise->dates().front();
        const Real K = opt.payoff->strike();
        const bool is_call = (opt.payoff->type() == OptionType::Call);

        // ── Black-Scholes d1, d2 ───────────────────────────────────────────────
        const Real sqrtT = std::sqrt(T);
        const Real stddev = v * sqrtT;
        const Real d1 = (std::log(S / K) + (r - q + 0.5 * v * v) * T) / stddev;
        const Real d2 = d1 - stddev;

        // ── Discount factors ──────────────────────────────────────────────────
        const Real df_r = std::exp(-r * T);
        const Real df_q = std::exp(-q * T);

        // ── Price ─────────────────────────────────────────────────────────────
        Real npv = 0.0;
        std::string diag;

        if (opt.payoff_type == DigitalPayoffType::CashOrNothing)
        {
            // Call: cash * e^(-rT) * N(d2)
            // Put:  cash * e^(-rT) * N(-d2)
            const Real nd = is_call ? norm_cdf(d2) : norm_cdf(-d2);
            npv = opt.notional * opt.cash_amount * df_r * nd;
            diag = std::string("Digital cash-or-nothing ") + (is_call ? "call" : "put") +
                   " (BS analytic): cash=" + std::to_string(opt.cash_amount) +
                   ", K=" + std::to_string(K) + ", T=" + std::to_string(T);
        }
        else // AssetOrNothing
        {
            // Call: S * e^(-qT) * N(d1)
            // Put:  S * e^(-qT) * N(-d1)
            const Real nd = is_call ? norm_cdf(d1) : norm_cdf(-d1);
            npv = opt.notional * S * df_q * nd;
            diag = std::string("Digital asset-or-nothing ") + (is_call ? "call" : "put") +
                   " (BS analytic): K=" + std::to_string(K) + ", T=" + std::to_string(T);
        }

        PricingResult out;
        out.npv = npv;
        out.diagnostics = diag;
        // Greeks not computed for digitals (discontinuous payoff — special treatment needed)
        res_ = out;
    }

} // namespace quantModeling
