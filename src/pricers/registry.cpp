#include "quantModeling/pricers/registry.hpp"

#include "quantModeling/pricers/adapters/bonds.hpp"
#include "quantModeling/pricers/adapters/equity_asian.hpp"
#include "quantModeling/pricers/adapters/equity_future.hpp"
#include "quantModeling/pricers/adapters/equity_vanilla.hpp"
#include "quantModeling/pricers/adapters/equity_vanilla_american.hpp"

namespace quantModeling
{

    size_t RegistryKeyHash::operator()(const RegistryKey &key) const noexcept
    {
        const auto instrument = static_cast<size_t>(key.instrument);
        const auto model = static_cast<size_t>(key.model);
        const auto engine = static_cast<size_t>(key.engine);
        return (instrument << 16) ^ (model << 8) ^ engine;
    }

    void PricingRegistry::register_pricer(const RegistryKey &key, PricingFn fn)
    {
        registry_[key] = std::move(fn);
    }

    PricingResult PricingRegistry::price(const PricingRequest &request) const
    {
        RegistryKey key{request.instrument, request.model, request.engine};
        const auto it = registry_.find(key);
        if (it == registry_.end())
        {
            throw UnsupportedInstrument("No pricer registered for the requested instrument/model/engine.");
        }
        return it->second(request);
    }

    const PricingRegistry &default_registry()
    {
        static PricingRegistry registry = []()
        {
            PricingRegistry r;

            r.register_pricer(
                {InstrumentKind::EquityVanillaOption, ModelKind::BlackScholes, EngineKind::Analytic},
                [](const PricingRequest &request)
                {
                    const auto &in = std::get<VanillaBSInput>(request.input);
                    return price_equity_vanilla_bs(in, EngineKind::Analytic);
                });

            r.register_pricer(
                {InstrumentKind::EquityVanillaOption, ModelKind::BlackScholes, EngineKind::MonteCarlo},
                [](const PricingRequest &request)
                {
                    const auto &in = std::get<VanillaBSInput>(request.input);
                    return price_equity_vanilla_bs(in, EngineKind::MonteCarlo);
                });

            r.register_pricer(
                {InstrumentKind::EquityVanillaOption, ModelKind::BlackScholes, EngineKind::PDEFiniteDifference},
                [](const PricingRequest &request)
                {
                    const auto &in = std::get<VanillaBSInput>(request.input);
                    return price_equity_vanilla_bs(in, EngineKind::PDEFiniteDifference);
                });

            r.register_pricer(
                {InstrumentKind::EquityVanillaOption, ModelKind::BlackScholes, EngineKind::BinomialTree},
                [](const PricingRequest &request)
                {
                    const auto &in = std::get<VanillaBSInput>(request.input);
                    return price_equity_vanilla_bs(in, EngineKind::BinomialTree);
                });

            r.register_pricer(
                {InstrumentKind::EquityVanillaOption, ModelKind::BlackScholes, EngineKind::TrinomialTree},
                [](const PricingRequest &request)
                {
                    const auto &in = std::get<VanillaBSInput>(request.input);
                    return price_equity_vanilla_bs(in, EngineKind::TrinomialTree);
                });

            r.register_pricer(
                {InstrumentKind::EquityAmericanVanillaOption, ModelKind::BlackScholes, EngineKind::BinomialTree},
                [](const PricingRequest &request)
                {
                    const auto &in = std::get<AmericanVanillaBSInput>(request.input);
                    return price_equity_vanilla_american_bs(in, EngineKind::BinomialTree);
                });

            r.register_pricer(
                {InstrumentKind::EquityAmericanVanillaOption, ModelKind::BlackScholes, EngineKind::TrinomialTree},
                [](const PricingRequest &request)
                {
                    const auto &in = std::get<AmericanVanillaBSInput>(request.input);
                    return price_equity_vanilla_american_bs(in, EngineKind::TrinomialTree);
                });

            r.register_pricer(
                {InstrumentKind::EquityAmericanVanillaOption, ModelKind::BlackScholes, EngineKind::PDEFiniteDifference},
                [](const PricingRequest &request)
                {
                    const auto &in = std::get<AmericanVanillaBSInput>(request.input);
                    return price_equity_vanilla_american_bs(in, EngineKind::PDEFiniteDifference);
                });

            r.register_pricer(
                {InstrumentKind::EquityAsianOption, ModelKind::BlackScholes, EngineKind::Analytic},
                [](const PricingRequest &request)
                {
                    const auto &in = std::get<AsianBSInput>(request.input);
                    return price_equity_asian_bs(in, EngineKind::Analytic);
                });

            r.register_pricer(
                {InstrumentKind::EquityAsianOption, ModelKind::BlackScholes, EngineKind::MonteCarlo},
                [](const PricingRequest &request)
                {
                    const auto &in = std::get<AsianBSInput>(request.input);
                    return price_equity_asian_bs(in, EngineKind::MonteCarlo);
                });

            r.register_pricer(
                {InstrumentKind::EquityFuture, ModelKind::BlackScholes, EngineKind::Analytic},
                [](const PricingRequest &request)
                {
                    const auto &in = std::get<EquityFutureInput>(request.input);
                    return price_equity_future_bs(in);
                });

            r.register_pricer(
                {InstrumentKind::ZeroCouponBond, ModelKind::FlatRate, EngineKind::Analytic},
                [](const PricingRequest &request)
                {
                    const auto &in = std::get<ZeroCouponBondInput>(request.input);
                    return price_zero_coupon_bond_flat(in);
                });

            r.register_pricer(
                {InstrumentKind::FixedRateBond, ModelKind::FlatRate, EngineKind::Analytic},
                [](const PricingRequest &request)
                {
                    const auto &in = std::get<FixedRateBondInput>(request.input);
                    return price_fixed_rate_bond_flat(in);
                });

            return r;
        }();

        return registry;
    }

} // namespace quantModeling
