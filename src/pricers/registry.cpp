#include "quantModeling/pricers/registry.hpp"

#include "quantModeling/pricers/adapters/bonds.hpp"
#include "quantModeling/pricers/adapters/equity_asian.hpp"
#include "quantModeling/pricers/adapters/equity_asian_lv.hpp"
#include "quantModeling/pricers/adapters/equity_barrier.hpp"
#include "quantModeling/pricers/adapters/equity_barrier_lv.hpp"
#include "quantModeling/pricers/adapters/equity_basket.hpp"
#include "quantModeling/pricers/adapters/equity_digital.hpp"
#include "quantModeling/pricers/adapters/equity_future.hpp"
#include "quantModeling/pricers/adapters/equity_lookback.hpp"
#include "quantModeling/pricers/adapters/equity_lookback_lv.hpp"
#include "quantModeling/pricers/adapters/equity_vanilla.hpp"
#include "quantModeling/pricers/adapters/equity_vanilla_american.hpp"
#include "quantModeling/pricers/adapters/rates_short_rate.hpp"
#include "quantModeling/pricers/adapters/equity_autocall.hpp"
#include "quantModeling/pricers/adapters/equity_mountain.hpp"
#include "quantModeling/pricers/adapters/equity_vol_swap.hpp"
#include "quantModeling/pricers/adapters/equity_dispersion.hpp"
#include "quantModeling/pricers/adapters/fx.hpp"
#include "quantModeling/pricers/adapters/commodity.hpp"
#include "quantModeling/pricers/adapters/equity_rainbow.hpp"

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
                {InstrumentKind::EquityBarrierOption, ModelKind::BlackScholes, EngineKind::MonteCarlo},
                [](const PricingRequest &request)
                {
                    const auto &in = std::get<BarrierBSInput>(request.input);
                    return price_equity_barrier_bs_mc(in);
                });

            r.register_pricer(
                {InstrumentKind::EquityDigitalOption, ModelKind::BlackScholes, EngineKind::Analytic},
                [](const PricingRequest &request)
                {
                    const auto &in = std::get<DigitalBSInput>(request.input);
                    return price_equity_digital_bs_analytic(in);
                });

            r.register_pricer(
                {InstrumentKind::EquityLookbackOption, ModelKind::BlackScholes, EngineKind::MonteCarlo},
                [](const PricingRequest &request)
                {
                    const auto &in = std::get<LookbackBSInput>(request.input);
                    return price_equity_lookback_bs_mc(in);
                });

            r.register_pricer(
                {InstrumentKind::EquityBasketOption, ModelKind::BlackScholes, EngineKind::MonteCarlo},
                [](const PricingRequest &request)
                {
                    const auto &in = std::get<BasketBSInput>(request.input);
                    return price_equity_basket_bs_mc(in);
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

            r.register_pricer(
                {InstrumentKind::EquityBarrierOption, ModelKind::DupireLocalVol, EngineKind::MonteCarlo},
                [](const PricingRequest &request)
                {
                    const auto &in = std::get<BarrierLocalVolInput>(request.input);
                    return price_equity_barrier_lv_mc(in);
                });

            r.register_pricer(
                {InstrumentKind::EquityLookbackOption, ModelKind::DupireLocalVol, EngineKind::MonteCarlo},
                [](const PricingRequest &request)
                {
                    const auto &in = std::get<LookbackLocalVolInput>(request.input);
                    return price_equity_lookback_lv_mc(in);
                });

            r.register_pricer(
                {InstrumentKind::EquityAsianOption, ModelKind::DupireLocalVol, EngineKind::MonteCarlo},
                [](const PricingRequest &request)
                {
                    const auto &in = std::get<AsianLocalVolInput>(request.input);
                    return price_equity_asian_lv_mc(in);
                });

            // ── Short-rate: ZCB ──────────────────────────────────────────────

            for (auto mk : {ModelKind::Vasicek, ModelKind::CIR, ModelKind::HullWhite})
            {
                r.register_pricer(
                    {InstrumentKind::ZeroCouponBond, mk, EngineKind::Analytic},
                    [](const PricingRequest &request)
                    {
                        const auto &in = std::get<ShortRateZCBInput>(request.input);
                        return price_zcb_short_rate(in);
                    });
            }

            // ── Short-rate: FixedRateBond ────────────────────────────────────

            for (auto mk : {ModelKind::Vasicek, ModelKind::CIR, ModelKind::HullWhite})
            {
                r.register_pricer(
                    {InstrumentKind::FixedRateBond, mk, EngineKind::Analytic},
                    [](const PricingRequest &request)
                    {
                        const auto &in = std::get<ShortRateBondInput>(request.input);
                        return price_fixed_bond_short_rate(in);
                    });
            }

            // ── Short-rate: BondOption — analytic ────────────────────────────

            for (auto mk : {ModelKind::Vasicek, ModelKind::CIR, ModelKind::HullWhite})
            {
                r.register_pricer(
                    {InstrumentKind::BondOption, mk, EngineKind::Analytic},
                    [](const PricingRequest &request)
                    {
                        const auto &in = std::get<ShortRateBondOptionInput>(request.input);
                        return price_bond_option_short_rate_analytic(in);
                    });
            }

            // ── Short-rate: BondOption — MC ──────────────────────────────────

            for (auto mk : {ModelKind::Vasicek, ModelKind::CIR, ModelKind::HullWhite})
            {
                r.register_pricer(
                    {InstrumentKind::BondOption, mk, EngineKind::MonteCarlo},
                    [](const PricingRequest &request)
                    {
                        const auto &in = std::get<ShortRateBondOptionInput>(request.input);
                        return price_bond_option_short_rate_mc(in);
                    });
            }

            // ── Short-rate: CapFloor — analytic ──────────────────────────────

            for (auto mk : {ModelKind::Vasicek, ModelKind::CIR, ModelKind::HullWhite})
            {
                r.register_pricer(
                    {InstrumentKind::CapFloor, mk, EngineKind::Analytic},
                    [](const PricingRequest &request)
                    {
                        const auto &in = std::get<ShortRateCapFloorInput>(request.input);
                        return price_capfloor_short_rate_analytic(in);
                    });
            }

            // ── Short-rate: CapFloor — MC ────────────────────────────────────

            for (auto mk : {ModelKind::Vasicek, ModelKind::CIR, ModelKind::HullWhite})
            {
                r.register_pricer(
                    {InstrumentKind::CapFloor, mk, EngineKind::MonteCarlo},
                    [](const PricingRequest &request)
                    {
                        const auto &in = std::get<ShortRateCapFloorInput>(request.input);
                        return price_capfloor_short_rate_mc(in);
                    });
            }

            // ── Short-rate: Caplet — analytic ────────────────────────────────

            for (auto mk : {ModelKind::Vasicek, ModelKind::CIR, ModelKind::HullWhite})
            {
                r.register_pricer(
                    {InstrumentKind::Caplet, mk, EngineKind::Analytic},
                    [](const PricingRequest &request)
                    {
                        const auto &in = std::get<ShortRateCapletInput>(request.input);
                        return price_caplet_short_rate_analytic(in);
                    });
            }

            // ── Short-rate: Caplet — MC ──────────────────────────────────────

            for (auto mk : {ModelKind::Vasicek, ModelKind::CIR, ModelKind::HullWhite})
            {
                r.register_pricer(
                    {InstrumentKind::Caplet, mk, EngineKind::MonteCarlo},
                    [](const PricingRequest &request)
                    {
                        const auto &in = std::get<ShortRateCapletInput>(request.input);
                        return price_caplet_short_rate_mc(in);
                    });
            }

            // ── Equity: Autocall — MC ────────────────────────────────────────

            r.register_pricer(
                {InstrumentKind::Autocall, ModelKind::BlackScholes, EngineKind::MonteCarlo},
                [](const PricingRequest &request)
                {
                    const auto &in = std::get<AutocallBSInput>(request.input);
                    return price_equity_autocall_bs_mc(in);
                });

            // ── Equity: Mountain (Himalaya) — MC ─────────────────────────────

            r.register_pricer(
                {InstrumentKind::Mountain, ModelKind::BlackScholes, EngineKind::MonteCarlo},
                [](const PricingRequest &request)
                {
                    const auto &in = std::get<MountainBSInput>(request.input);
                    return price_equity_mountain_bs_mc(in);
                });

            // ── Volatility: Variance Swap — Analytic ─────────────────────────

            r.register_pricer(
                {InstrumentKind::VarianceSwap, ModelKind::BlackScholes, EngineKind::Analytic},
                [](const PricingRequest &request)
                {
                    const auto &in = std::get<VarianceSwapBSInput>(request.input);
                    return price_variance_swap_bs_analytic(in);
                });

            // ── Volatility: Variance Swap — MC ──────────────────────────────

            r.register_pricer(
                {InstrumentKind::VarianceSwap, ModelKind::BlackScholes, EngineKind::MonteCarlo},
                [](const PricingRequest &request)
                {
                    const auto &in = std::get<VarianceSwapBSInput>(request.input);
                    return price_variance_swap_bs_mc(in);
                });

            // ── Volatility: Volatility Swap — MC ─────────────────────────────

            r.register_pricer(
                {InstrumentKind::VolatilitySwap, ModelKind::BlackScholes, EngineKind::MonteCarlo},
                [](const PricingRequest &request)
                {
                    const auto &in = std::get<VolatilitySwapBSInput>(request.input);
                    return price_volatility_swap_bs_mc(in);
                });

            // ── Dispersion: Dispersion Swap — MC ─────────────────────────────

            r.register_pricer(
                {InstrumentKind::DispersionSwap, ModelKind::BlackScholes, EngineKind::MonteCarlo},
                [](const PricingRequest &request)
                {
                    const auto &in = std::get<DispersionBSInput>(request.input);
                    return price_dispersion_bs_mc(in);
                });

            // ── FX: Forward — Analytic ───────────────────────────────────────

            r.register_pricer(
                {InstrumentKind::FXForward, ModelKind::GarmanKohlhagen, EngineKind::Analytic},
                [](const PricingRequest &request)
                {
                    const auto &in = std::get<FXForwardInput>(request.input);
                    return price_fx_forward_analytic(in);
                });

            // ── FX: Option — Analytic (Garman-Kohlhagen) ─────────────────────

            r.register_pricer(
                {InstrumentKind::FXOption, ModelKind::GarmanKohlhagen, EngineKind::Analytic},
                [](const PricingRequest &request)
                {
                    const auto &in = std::get<FXOptionInput>(request.input);
                    return price_fx_option_analytic(in);
                });

            // ── Commodity: Forward — Analytic ────────────────────────────────

            r.register_pricer(
                {InstrumentKind::CommodityForward, ModelKind::CommodityBlack, EngineKind::Analytic},
                [](const PricingRequest &request)
                {
                    const auto &in = std::get<CommodityForwardInput>(request.input);
                    return price_commodity_forward_analytic(in);
                });

            // ── Commodity: Option — Analytic (Black '76) ────────────────────

            r.register_pricer(
                {InstrumentKind::CommodityOption, ModelKind::CommodityBlack, EngineKind::Analytic},
                [](const PricingRequest &request)
                {
                    const auto &in = std::get<CommodityOptionInput>(request.input);
                    return price_commodity_option_analytic(in);
                });

            // ── Rainbow: Worst-of Option — MC ─────────────────────────

            r.register_pricer(
                {InstrumentKind::WorstOfOption, ModelKind::BlackScholes, EngineKind::MonteCarlo},
                [](const PricingRequest &request)
                {
                    const auto &in = std::get<RainbowBSInput>(request.input);
                    return price_worst_of_bs_mc(in);
                });

            // ── Rainbow: Best-of Option — MC ──────────────────────────

            r.register_pricer(
                {InstrumentKind::BestOfOption, ModelKind::BlackScholes, EngineKind::MonteCarlo},
                [](const PricingRequest &request)
                {
                    const auto &in = std::get<RainbowBSInput>(request.input);
                    return price_best_of_bs_mc(in);
                });

            return r;
        }();

        return registry;
    }

} // namespace quantModeling
