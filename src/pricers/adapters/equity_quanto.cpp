#include "quantModeling/pricers/adapters/equity_quanto.hpp"

#include "quantModeling/engines/analytic/black_scholes.hpp"
#include "quantModeling/engines/mc/black_scholes.hpp"
#include "quantModeling/instruments/equity/vanilla.hpp"
#include "quantModeling/models/equity/quanto_black_scholes.hpp"
#include "quantModeling/pricers/context.hpp"
#include "quantModeling/pricers/pricer.hpp"

#include <cmath>
#include <memory>

namespace quantModeling
{

    PricingResult price_quanto_vanilla_bs(const QuantoBSInput &in, EngineKind engine)
    {
        if (!(in.vol > 0.0) || !(in.fx_vol > 0.0))
            throw InvalidInput("quanto: vol and fx_vol must be > 0");
        if (!(in.correlation >= -1.0 && in.correlation <= 1.0))
            throw InvalidInput("quanto: correlation must be in [-1, 1]");
        if (!(in.fx_rate > 0.0))
            throw InvalidInput("quanto: fx_rate (the fixed conversion rate) must be > 0");

        auto payoff = std::make_shared<PlainVanillaPayoff>(
            in.is_call ? OptionType::Call : OptionType::Put, in.strike);
        auto exercise = std::make_shared<EuropeanExercise>(in.maturity);
        // The fixed conversion rate is the notional: every unit of foreign
        // payoff is paid as X̄ units of the domestic currency.
        VanillaOption opt(payoff, exercise, in.fx_rate);

        auto model = std::make_shared<QuantoBlackScholesModel>(
            in.spot, in.rate_domestic, in.rate_foreign, in.dividend, in.vol, in.fx_vol,
            in.correlation);

        const bool mc = engine == EngineKind::MonteCarlo;
        PricingSettings settings = {mc ? in.n_paths : 0, mc ? in.seed : 0, true, 100, 100, 100};
        PricingContext ctx{MarketView{}, settings, model};

        PricingResult out;
        if (mc)
        {
            BSEuroVanillaMCEngine e(ctx);
            out = price(opt, e);
        }
        else
        {
            BSEuroVanillaAnalyticEngine e(ctx);
            out = price(opt, e);
        }

        // The engines differentiate with q_eff = r_d − r_f + q + ρσ_Sσ_X held
        // fixed; the quanto's own sensitivities move it:
        //  - r_d: the drift r_d − q_eff = r_f − q − ρσ_Sσ_X does not depend on
        //    r_d, so only the discounting does: ∂V/∂r_d = −T·V.
        //  - σ_S: ∂V/∂σ_S = vega|q_eff + (∂V/∂q_eff)·ρσ_X, and for a European
        //    option ∂V/∂q = −T·S·Δ.
        const Real T = in.maturity;
        out.greeks.rho = -T * out.npv;
        if (mc)
            out.greeks.rho_std_error = T * out.mc_std_error;
        if (out.greeks.vega && out.greeks.delta)
            out.greeks.vega = *out.greeks.vega -
                              T * in.spot * *out.greeks.delta * in.correlation * in.fx_vol;
        out.diagnostics += (out.diagnostics.empty() ? "" : " | ") +
                           std::string("quanto drift adjustment rho*sigma_S*sigma_X = ") +
                           std::to_string(model->quanto_adjustment()) +
                           "; rho is the domestic-rate rho";
        return out;
    }

} // namespace quantModeling
