#include "quantModeling/pricers/registry.hpp"

#include "quantModeling/pricers/adapters/equity_asian.hpp"
#include "quantModeling/pricers/adapters/equity_vanilla.hpp"

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

            return r;
        }();

        return registry;
    }

} // namespace quantModeling
