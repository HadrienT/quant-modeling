#include "quantModeling/pricers/adapters/equity_asian.hpp"

#include "quantModeling/engines/analytic/asian.hpp"
#include "quantModeling/engines/mc/asian.hpp"
#include "quantModeling/instruments/equity/asian.hpp"
#include "quantModeling/models/equity/black_scholes.hpp"
#include "quantModeling/pricers/context.hpp"
#include "quantModeling/pricers/pricer.hpp"

#include <memory>

namespace quantModeling
{

    PricingResult price_equity_asian_bs(const AsianBSInput &in, EngineKind engine)
    {
        std::shared_ptr<IPayoff> payoff;
        if (in.average_type == AsianAverageType::Arithmetic)
        {
            payoff = std::make_shared<ArithmeticAsianPayoff>(
                in.is_call ? OptionType::Call : OptionType::Put,
                static_cast<Real>(in.strike));
        }
        else
        {
            payoff = std::make_shared<GeometricAsianPayoff>(
                in.is_call ? OptionType::Call : OptionType::Put,
                static_cast<Real>(in.strike));
        }

        auto exercise = std::make_shared<EuropeanExercise>(
            static_cast<Real>(in.maturity));

        AsianOption opt(payoff, exercise, in.average_type, 1.0);

        auto model = std::make_shared<BlackScholesModel>(
            static_cast<Real>(in.spot),
            static_cast<Real>(in.rate),
            static_cast<Real>(in.dividend),
            static_cast<Real>(in.vol));

        MarketView market = {};
        const bool use_mc = engine == EngineKind::MonteCarlo;
        PricingSettings settings = {
            use_mc ? in.n_paths : 0,
            use_mc ? in.seed : 0,
            true,
            0,  // tree_steps
            0,  // pde_space_steps
            0}; // pde_time_steps

        PricingContext ctx{market, settings, model};

        if (use_mc)
        {
            BSEuroAsianMCEngine mc_engine(ctx);
            return price(opt, mc_engine);
        }

        if (in.average_type == AsianAverageType::Arithmetic)
        {
            BSEuroArithmeticAsianAnalyticEngine analytic_engine(ctx);
            return price(opt, analytic_engine);
        }

        BSEuroGeometricAsianAnalyticEngine analytic_engine(ctx);
        return price(opt, analytic_engine);
    }

} // namespace quantModeling
