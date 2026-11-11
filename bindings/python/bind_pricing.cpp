// Python bindings, pricing (split from src/module.cpp: one translation unit per
// domain, so that a header change recompiles only the bindings that use it).

#include "common.hpp"

#include "quantModeling/pricers/inputs.hpp"
#include "quantModeling/pricers/registry.hpp"
#include "quantModeling/engines/mc/local_vol.hpp"
#include "quantModeling/aad/number.hpp"
#include "quantModeling/engines/mc/simulation_engine.hpp"
#include "quantModeling/engines/xva/cross_currency_exposure_engine.hpp"
#include "quantModeling/instruments/fx/forward.hpp"
#include "quantModeling/engines/mc/lsm.hpp"
#include "quantModeling/scripting/model_advice.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <tuple>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace py = pybind11;

namespace py = pybind11;

namespace quantModeling
{

    static PricingResult price_vanilla_impl(const VanillaBSInput &in, bool use_mc)
    {
        const EngineKind engine = use_mc ? EngineKind::MonteCarlo : EngineKind::Analytic;
        PricingRequest request{
            InstrumentKind::EquityVanillaOption,
            ModelKind::BlackScholes,
            engine,
            PricingInput{in}};
        return default_registry().price(request);
    }

    static PricingResult price_quanto_impl(const QuantoBSInput &in, bool use_mc)
    {
        PricingRequest request{InstrumentKind::EquityVanillaOption,
                               ModelKind::QuantoBlackScholes,
                               use_mc ? EngineKind::MonteCarlo : EngineKind::Analytic,
                               PricingInput{in}};
        return default_registry().price(request);
    }

    static PricingResult price_asian_impl(const AsianBSInput &in, bool use_mc)
    {
        const EngineKind engine = use_mc ? EngineKind::MonteCarlo : EngineKind::Analytic;
        PricingRequest request{
            InstrumentKind::EquityAsianOption,
            ModelKind::BlackScholes,
            engine,
            PricingInput{in}};
        return default_registry().price(request);
    }

    static PricingResult price_zero_coupon_impl(const ZeroCouponBondInput &in)
    {
        PricingRequest request{
            InstrumentKind::ZeroCouponBond,
            ModelKind::FlatRate,
            EngineKind::Analytic,
            PricingInput{in}};
        return default_registry().price(request);
    }

    static PricingResult price_fixed_rate_impl(const FixedRateBondInput &in)
    {
        PricingRequest request{
            InstrumentKind::FixedRateBond,
            ModelKind::FlatRate,
            EngineKind::Analytic,
            PricingInput{in}};
        return default_registry().price(request);
    }

    static PricingResult price_american_vanilla_impl(const AmericanVanillaBSInput &in, EngineKind engine)
    {
        PricingRequest request{
            InstrumentKind::EquityAmericanVanillaOption,
            ModelKind::BlackScholes,
            engine,
            PricingInput{in}};
        return default_registry().price(request);
    }

    static PricingResult price_vanilla_pde_impl(const VanillaBSInput &in)
    {
        PricingRequest request{
            InstrumentKind::EquityVanillaOption,
            ModelKind::BlackScholes,
            EngineKind::PDEFiniteDifference,
            PricingInput{in}};
        return default_registry().price(request);
    }

    static PricingResult price_barrier_impl(const BarrierBSInput &in)
    {
        PricingRequest request{
            InstrumentKind::EquityBarrierOption,
            ModelKind::BlackScholes,
            EngineKind::MonteCarlo,
            PricingInput{in}};
        return default_registry().price(request);
    }

    static PricingResult price_digital_impl(const DigitalBSInput &in)
    {
        PricingRequest request{
            InstrumentKind::EquityDigitalOption,
            ModelKind::BlackScholes,
            EngineKind::Analytic,
            PricingInput{in}};
        return default_registry().price(request);
    }

    static PricingResult price_lookback_impl(const LookbackBSInput &in)
    {
        PricingRequest request{
            InstrumentKind::EquityLookbackOption,
            ModelKind::BlackScholes,
            EngineKind::MonteCarlo,
            PricingInput{in}};
        return default_registry().price(request);
    }

    static PricingResult price_basket_impl(const BasketBSInput &in)
    {
        PricingRequest request{
            InstrumentKind::EquityBasketOption,
            ModelKind::BlackScholes,
            EngineKind::MonteCarlo,
            PricingInput{in}};
        return default_registry().price(request);
    }

    static PricingResult price_barrier_lv_impl(const BarrierLocalVolInput &in)
    {
        PricingRequest request{
            InstrumentKind::EquityBarrierOption,
            ModelKind::DupireLocalVol,
            EngineKind::MonteCarlo,
            PricingInput{in}};
        return default_registry().price(request);
    }

    static PricingResult price_lookback_lv_impl(const LookbackLocalVolInput &in)
    {
        PricingRequest request{
            InstrumentKind::EquityLookbackOption,
            ModelKind::DupireLocalVol,
            EngineKind::MonteCarlo,
            PricingInput{in}};
        return default_registry().price(request);
    }

    static PricingResult price_asian_lv_impl(const AsianLocalVolInput &in)
    {
        PricingRequest request{
            InstrumentKind::EquityAsianOption,
            ModelKind::DupireLocalVol,
            EngineKind::MonteCarlo,
            PricingInput{in}};
        return default_registry().price(request);
    }

    // ── Autocall ────────────────────────────────────────────────────────

    static PricingResult price_autocall_impl(const AutocallBSInput &in)
    {
        PricingRequest request{
            InstrumentKind::Autocall,
            ModelKind::BlackScholes,
            EngineKind::MonteCarlo,
            PricingInput{in}};
        return default_registry().price(request);
    }

    // ── Mountain (Himalaya) ─────────────────────────────────────────────

    static PricingResult price_mountain_impl(const MountainBSInput &in)
    {
        PricingRequest request{
            InstrumentKind::Mountain,
            ModelKind::BlackScholes,
            EngineKind::MonteCarlo,
            PricingInput{in}};
        return default_registry().price(request);
    }

    // ── Variance Swap ───────────────────────────────────────────────────

    static PricingResult price_variance_swap_analytic_impl(const VarianceSwapBSInput &in)
    {
        PricingRequest request{
            InstrumentKind::VarianceSwap,
            ModelKind::BlackScholes,
            EngineKind::Analytic,
            PricingInput{in}};
        return default_registry().price(request);
    }

    static PricingResult price_variance_swap_mc_impl(const VarianceSwapBSInput &in)
    {
        PricingRequest request{
            InstrumentKind::VarianceSwap,
            ModelKind::BlackScholes,
            EngineKind::MonteCarlo,
            PricingInput{in}};
        return default_registry().price(request);
    }

    // ── Volatility Swap ─────────────────────────────────────────────────

    static PricingResult price_volatility_swap_mc_impl(const VolatilitySwapBSInput &in)
    {
        PricingRequest request{
            InstrumentKind::VolatilitySwap,
            ModelKind::BlackScholes,
            EngineKind::MonteCarlo,
            PricingInput{in}};
        return default_registry().price(request);
    }

    // ── Dispersion Swap ─────────────────────────────────────────────────

    static PricingResult price_dispersion_mc_impl(const DispersionBSInput &in)
    {
        PricingRequest request{
            InstrumentKind::DispersionSwap,
            ModelKind::BlackScholes,
            EngineKind::MonteCarlo,
            PricingInput{in}};
        return default_registry().price(request);
    }

    // ── FX Forward ──────────────────────────────────────────────────────

    static PricingResult price_fx_forward_impl(const FXForwardInput &in)
    {
        PricingRequest request{
            InstrumentKind::FXForward,
            ModelKind::GarmanKohlhagen,
            EngineKind::Analytic,
            PricingInput{in}};
        return default_registry().price(request);
    }

    // ── FX Option ───────────────────────────────────────────────────────

    static PricingResult price_fx_option_impl(const FXOptionInput &in)
    {
        PricingRequest request{
            InstrumentKind::FXOption,
            ModelKind::GarmanKohlhagen,
            EngineKind::Analytic,
            PricingInput{in}};
        return default_registry().price(request);
    }

    // ── Commodity Forward ───────────────────────────────────────────────

    static PricingResult price_commodity_forward_impl(const CommodityForwardInput &in)
    {
        PricingRequest request{
            InstrumentKind::CommodityForward,
            ModelKind::CommodityBlack,
            EngineKind::Analytic,
            PricingInput{in}};
        return default_registry().price(request);
    }

    // ── Commodity Option ────────────────────────────────────────────────

    static PricingResult price_commodity_option_impl(const CommodityOptionInput &in)
    {
        PricingRequest request{
            InstrumentKind::CommodityOption,
            ModelKind::CommodityBlack,
            EngineKind::Analytic,
            PricingInput{in}};
        return default_registry().price(request);
    }

    // ── Worst-of Option ─────────────────────────────────────────────────

    static PricingResult price_worst_of_impl(const RainbowBSInput &in)
    {
        PricingRequest request{
            InstrumentKind::WorstOfOption,
            ModelKind::BlackScholes,
            EngineKind::MonteCarlo,
            PricingInput{in}};
        return default_registry().price(request);
    }

    // ── Best-of Option ──────────────────────────────────────────────────

    static PricingResult price_best_of_impl(const RainbowBSInput &in)
    {
        PricingRequest request{
            InstrumentKind::BestOfOption,
            ModelKind::BlackScholes,
            EngineKind::MonteCarlo,
            PricingInput{in}};
        return default_registry().price(request);
    }

} // namespace quantModeling

static py::dict price_vanilla_bs_analytic(const quantModeling::VanillaBSInput &in)
{
    auto res = quantModeling::price_vanilla_impl(in, false);
    return pricing_result_to_dict(res);
}

static py::dict price_vanilla_bs_mc(const quantModeling::VanillaBSInput &in)
{
    auto res = quantModeling::price_vanilla_impl(in, true);
    return pricing_result_to_dict(res);
}

static py::dict price_vanilla_bs_pde(const quantModeling::VanillaBSInput &in)
{
    auto res = quantModeling::price_vanilla_pde_impl(in);
    return pricing_result_to_dict(res);
}

static py::dict price_vanilla_bs_binomial(const quantModeling::VanillaBSInput &in)
{
    quantModeling::PricingRequest request{
        quantModeling::InstrumentKind::EquityVanillaOption,
        quantModeling::ModelKind::BlackScholes,
        quantModeling::EngineKind::BinomialTree,
        quantModeling::PricingInput{in}};
    auto res = quantModeling::default_registry().price(request);
    return pricing_result_to_dict(res);
}

static py::dict price_vanilla_bs_trinomial(const quantModeling::VanillaBSInput &in)
{
    quantModeling::PricingRequest request{
        quantModeling::InstrumentKind::EquityVanillaOption,
        quantModeling::ModelKind::BlackScholes,
        quantModeling::EngineKind::TrinomialTree,
        quantModeling::PricingInput{in}};
    auto res = quantModeling::default_registry().price(request);
    return pricing_result_to_dict(res);
}

static py::dict price_american_vanilla_bs_binomial(const quantModeling::AmericanVanillaBSInput &in)
{
    auto res = quantModeling::price_american_vanilla_impl(in, quantModeling::EngineKind::BinomialTree);
    return pricing_result_to_dict(res);
}

static py::dict price_american_vanilla_bs_trinomial(const quantModeling::AmericanVanillaBSInput &in)
{
    auto res = quantModeling::price_american_vanilla_impl(in, quantModeling::EngineKind::TrinomialTree);
    return pricing_result_to_dict(res);
}

static py::dict price_asian_bs_analytic(const quantModeling::AsianBSInput &in)
{
    auto res = quantModeling::price_asian_impl(in, false);
    return pricing_result_to_dict(res);
}

static py::dict price_asian_bs_mc(const quantModeling::AsianBSInput &in)
{
    auto res = quantModeling::price_asian_impl(in, true);
    return pricing_result_to_dict(res);
}

static py::dict price_future_bs_analytic(const quantModeling::EquityFutureInput &in)
{
    quantModeling::PricingRequest request{
        quantModeling::InstrumentKind::EquityFuture,
        quantModeling::ModelKind::BlackScholes,
        quantModeling::EngineKind::Analytic,
        quantModeling::PricingInput{in}};
    auto res = quantModeling::default_registry().price(request);
    return pricing_result_to_dict(res);
}

static py::dict price_zero_coupon_bond_analytic(const quantModeling::ZeroCouponBondInput &in)
{
    auto res = quantModeling::price_zero_coupon_impl(in);
    return pricing_result_to_dict(res);
}

static py::dict price_fixed_rate_bond_analytic(const quantModeling::FixedRateBondInput &in)
{
    auto res = quantModeling::price_fixed_rate_impl(in);
    return pricing_result_to_dict(res);
}

static py::dict price_barrier_bs_mc(const quantModeling::BarrierBSInput &in)
{
    auto res = quantModeling::price_barrier_impl(in);
    return pricing_result_to_dict(res);
}

static py::dict price_digital_bs_analytic(const quantModeling::DigitalBSInput &in)
{
    auto res = quantModeling::price_digital_impl(in);
    return pricing_result_to_dict(res);
}

static py::dict price_lookback_bs_mc(const quantModeling::LookbackBSInput &in)
{
    auto res = quantModeling::price_lookback_impl(in);
    return pricing_result_to_dict(res);
}

static py::dict price_basket_bs_mc(const quantModeling::BasketBSInput &in)
{
    auto res = quantModeling::price_basket_impl(in);
    return pricing_result_to_dict(res);
}

static py::dict price_local_vol_mc_impl(const quantModeling::LocalVolInput &in)
{
    auto res = quantModeling::price_local_vol_mc(in);
    return pricing_result_to_dict(res);
}

void register_pricing(py::module_ &m)
{
    m.def("price_vanilla_bs_analytic", &price_vanilla_bs_analytic,
          "Price vanilla option under Black-Scholes (analytic).");
    m.def("price_vanilla_bs_mc", &price_vanilla_bs_mc,
          "Price vanilla option under Black-Scholes (Monte Carlo).");
    m.def("price_vanilla_bs_pde", &price_vanilla_bs_pde,
          "Price vanilla option under Black-Scholes (PDE Crank-Nicolson, European only).");
    m.def("price_vanilla_bs_binomial", &price_vanilla_bs_binomial,
          "Price European vanilla option under Black-Scholes (Binomial tree).");
    m.def("price_vanilla_bs_trinomial", &price_vanilla_bs_trinomial,
          "Price European vanilla option under Black-Scholes (Trinomial tree).");
    m.def("price_american_vanilla_bs_binomial", &price_american_vanilla_bs_binomial,
          "Price American vanilla option under Black-Scholes (Binomial tree).");
    m.def("price_american_vanilla_bs_trinomial", &price_american_vanilla_bs_trinomial,
          "Price American vanilla option under Black-Scholes (Trinomial tree).");
    m.def("price_asian_bs_analytic", &price_asian_bs_analytic,
          "Price Asian option under Black-Scholes (analytic).");
    m.def("price_asian_bs_mc", &price_asian_bs_mc,
          "Price Asian option under Black-Scholes (Monte Carlo).");
    m.def("price_future_bs_analytic", &price_future_bs_analytic,
          "Price equity future under Black-Scholes (analytic).");
    m.def("price_zero_coupon_bond_analytic", &price_zero_coupon_bond_analytic,
          "Price zero coupon bond under flat-rate discounting (analytic).");
    m.def("price_fixed_rate_bond_analytic", &price_fixed_rate_bond_analytic,
          "Price fixed-rate bond under flat-rate discounting (analytic).");
    m.def("price_barrier_bs_mc", &price_barrier_bs_mc,
          "Price barrier option under Black-Scholes (Monte Carlo, all four barrier types).");
    m.def("price_digital_bs_analytic", &price_digital_bs_analytic,
          "Price digital option under Black-Scholes (analytic, Cash-or-Nothing / Asset-or-Nothing).");
    m.def("price_lookback_bs_mc", &price_lookback_bs_mc,
          "Price lookback option under Black-Scholes (Monte Carlo).");
    m.def("price_basket_bs_mc", &price_basket_bs_mc,
          "Price basket option under correlated multi-asset Black-Scholes (Monte Carlo).");

    m.def("price_local_vol_mc", &price_local_vol_mc_impl,
          "Price a European vanilla option under a Dupire local-vol surface (C++ Euler-Maruyama MC).");

    // ── Vol surface calibration ──────────────────────────────────────────────────────
    m.def("price_barrier_lv_mc", [](const quantModeling::BarrierLocalVolInput &in)
          { return pricing_result_to_dict(quantModeling::price_barrier_lv_impl(in)); }, "Price a barrier option under a Dupire local-vol surface (Monte Carlo).");

    // ── LookbackLocalVolInput ────────────────────────────────────────────────────────────
    m.def("price_lookback_lv_mc", [](const quantModeling::LookbackLocalVolInput &in)
          { return pricing_result_to_dict(quantModeling::price_lookback_lv_impl(in)); }, "Price a lookback option under a Dupire local-vol surface (Monte Carlo).");

    // ── AsianLocalVolInput ─────────────────────────────────────────────────────────────────
    m.def("price_asian_lv_mc", [](const quantModeling::AsianLocalVolInput &in)
          { return pricing_result_to_dict(quantModeling::price_asian_lv_impl(in)); }, "Price an Asian option under a Dupire local-vol surface (Monte Carlo).");

    // ── Autocall ───────────────────────────────────────────────────────────────────
    m.def("price_autocall_bs_mc", [](const quantModeling::AutocallBSInput &in)
          { return pricing_result_to_dict(quantModeling::price_autocall_impl(in)); }, "Price an autocallable note under Black-Scholes (Monte Carlo).");

    // ── Mountain (Himalaya) ────────────────────────────────────────────────────────
    m.def("price_mountain_bs_mc", [](const quantModeling::MountainBSInput &in)
          { return pricing_result_to_dict(quantModeling::price_mountain_impl(in)); }, "Price a Himalaya (Mountain) option under multi-asset BS (Monte Carlo).");

    // ── Variance Swap ──────────────────────────────────────────────────────────────
    m.def("price_variance_swap_bs_analytic", [](const quantModeling::VarianceSwapBSInput &in)
          { return pricing_result_to_dict(quantModeling::price_variance_swap_analytic_impl(in)); }, "Price a variance swap under Black-Scholes (analytic).");

    m.def("price_variance_swap_bs_mc", [](const quantModeling::VarianceSwapBSInput &in)
          { return pricing_result_to_dict(quantModeling::price_variance_swap_mc_impl(in)); }, "Price a variance swap under Black-Scholes (Monte Carlo).");

    // ── Volatility Swap ────────────────────────────────────────────────────────────
    m.def("price_volatility_swap_bs_mc", [](const quantModeling::VolatilitySwapBSInput &in)
          { return pricing_result_to_dict(quantModeling::price_volatility_swap_mc_impl(in)); }, "Price a volatility swap under Black-Scholes (Monte Carlo).");

    // ── Dispersion Swap ────────────────────────────────────────────────────────────
    m.def("price_dispersion_bs_mc", [](const quantModeling::DispersionBSInput &in)
          { return pricing_result_to_dict(quantModeling::price_dispersion_mc_impl(in)); }, "Price a dispersion swap under multi-asset BS (Monte Carlo).");

    // ── FX Forward ─────────────────────────────────────────────────────────────────
    m.def("price_fx_forward_analytic", [](const quantModeling::FXForwardInput &in)
          { return pricing_result_to_dict(quantModeling::price_fx_forward_impl(in)); }, "Price an FX forward (analytic).");

    // ── FX Option ──────────────────────────────────────────────────────────────────
    m.def("price_quanto_bs_analytic", [](const quantModeling::QuantoBSInput &in)
          { return pricing_result_to_dict(quantModeling::price_quanto_impl(in, false)); }, "European quanto option, Black-Scholes with the quanto drift adjustment "
                                                                                           "(Reiner 1992), closed form. rho is the domestic-rate rho.");
    m.def("price_quanto_bs_mc", [](const quantModeling::QuantoBSInput &in)
          { return pricing_result_to_dict(quantModeling::price_quanto_impl(in, true)); }, "European quanto option, Black-Scholes with the quanto drift adjustment, "
                                                                                          "Monte Carlo.");

    m.def("price_fx_option_analytic", [](const quantModeling::FXOptionInput &in)
          { return pricing_result_to_dict(quantModeling::price_fx_option_impl(in)); }, "Price a European FX option (Garman-Kohlhagen analytic).");

    // ── Commodity Forward ──────────────────────────────────────────────────────────
    m.def("price_commodity_forward_analytic", [](const quantModeling::CommodityForwardInput &in)
          { return pricing_result_to_dict(quantModeling::price_commodity_forward_impl(in)); }, "Price a commodity forward (analytic).");

    // ── Commodity Option ───────────────────────────────────────────────────────────
    m.def("price_commodity_option_analytic", [](const quantModeling::CommodityOptionInput &in)
          { return pricing_result_to_dict(quantModeling::price_commodity_option_impl(in)); }, "Price a European commodity option (Black 76 analytic).");

    // ── Worst-of Option ────────────────────────────────────────────────────────────
    m.def("price_worst_of_bs_mc", [](const quantModeling::RainbowBSInput &in)
          { return pricing_result_to_dict(quantModeling::price_worst_of_impl(in)); }, "Price a worst-of option under multi-asset BS (Monte Carlo).");

    m.def("price_best_of_bs_mc", [](const quantModeling::RainbowBSInput &in)
          { return pricing_result_to_dict(quantModeling::price_best_of_impl(in)); }, "Price a best-of option under multi-asset BS (Monte Carlo).");

    // ── SABR calibration ──────────────────────────────────────────────────────────────
}
