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
        EquityBarrierOption,
        EquityDigitalOption,
        EquityLookbackOption,
        EquityBasketOption,
        EquityFuture,
        ZeroCouponBond,
        FixedRateBond,
        BondOption,
        CapFloor,
        Autocall,
        Mountain,
        Caplet,
        VarianceSwap,
        VolatilitySwap,
        DispersionSwap,
        FXForward,
        FXOption,
        CommodityForward,
        CommodityOption,
        WorstOfOption,
        BestOfOption
    };

    enum class ModelKind
    {
        BlackScholes,
        FlatRate,
        DupireLocalVol,
        Vasicek,
        CIR,
        HullWhite,
        GarmanKohlhagen,
        CommodityBlack
    };

    enum class EngineKind
    {
        Analytic,
        MonteCarlo,
        BinomialTree,
        TrinomialTree,
        PDEFiniteDifference
    };

    using PricingInput = std::variant<
        VanillaBSInput,
        AmericanVanillaBSInput,
        AsianBSInput,
        BarrierBSInput,
        DigitalBSInput,
        LookbackBSInput,
        BasketBSInput,
        EquityFutureInput,
        ZeroCouponBondInput,
        FixedRateBondInput,
        LocalVolInput,
        BarrierLocalVolInput,
        LookbackLocalVolInput,
        AsianLocalVolInput,
        ShortRateZCBInput,
        ShortRateBondInput,
        ShortRateBondOptionInput,
        ShortRateCapFloorInput,
        AutocallBSInput,
        MountainBSInput,
        ShortRateCapletInput,
        VarianceSwapBSInput,
        VolatilitySwapBSInput,
        DispersionBSInput,
        FXForwardInput,
        FXOptionInput,
        CommodityForwardInput,
        CommodityOptionInput,
        RainbowBSInput>;

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
