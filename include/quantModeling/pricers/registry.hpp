#ifndef PRICERS_REGISTRY_HPP
#define PRICERS_REGISTRY_HPP

#include "quantModeling/core/results.hpp"
#include "quantModeling/core/types.hpp"
#include "quantModeling/pricers/inputs.hpp"

#include <functional>
#include <unordered_map>
#include <variant>

namespace quantModeling
{

    enum class InstrumentKind
    {
        EquityVanillaOption,
        EquityAmericanVanillaOption,
        EquityAsianOption,
        EquityFuture,
        ZeroCouponBond,
        FixedRateBond
    };

    enum class ModelKind
    {
        BlackScholes,
        FlatRate
    };

    enum class EngineKind
    {
        Analytic,
        MonteCarlo,
        BinomialTree,
        TrinomialTree,
        PDEFiniteDifference
    };

    using PricingInput = std::variant<VanillaBSInput, AmericanVanillaBSInput, AsianBSInput, EquityFutureInput, ZeroCouponBondInput, FixedRateBondInput>;

    struct PricingRequest
    {
        InstrumentKind instrument;
        ModelKind model;
        EngineKind engine;
        PricingInput input;
    };

    struct RegistryKey
    {
        InstrumentKind instrument;
        ModelKind model;
        EngineKind engine;

        bool operator==(const RegistryKey &other) const = default;
    };

    struct RegistryKeyHash
    {
        size_t operator()(const RegistryKey &key) const noexcept;
    };

    using PricingFn = std::function<PricingResult(const PricingRequest &)>;

    class PricingRegistry
    {
    public:
        void register_pricer(const RegistryKey &key, PricingFn fn);
        PricingResult price(const PricingRequest &request) const;

    private:
        std::unordered_map<RegistryKey, PricingFn, RegistryKeyHash> registry_;
    };

    const PricingRegistry &default_registry();

} // namespace quantModeling

#endif
