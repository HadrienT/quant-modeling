from typing import Dict, List

import quantmodeling as qm

from .schemas import (
    AmericanEngineType,
    AmericanVanillaRequest,
    AsianAverageType,
    AsianRequest,
    AutocallRequest,
    BarrierKind,
    BarrierRequest,
    BasketRequest,
    CommodityForwardRequest,
    CommodityOptionRequest,
    DigitalPayoffKind,
    DigitalRequest,
    DispersionSwapRequest,
    EngineType,
    FixedRateBondRequest,
    FutureRequest,
    FXForwardRequest,
    FXOptionRequest,
    LookbackExtremum,
    LookbackRequest,
    LookbackStyle,
    MountainRequest,
    PricingResponse,
    RainbowKind,
    RainbowRequest,
    VanillaRequest,
    VarianceSwapRequest,
    VolatilitySwapRequest,
    ZeroCouponBondRequest,
)


def _pricing_response_from_dict(result: Dict) -> PricingResponse:
    return PricingResponse(
        npv=result["npv"],
        greeks=result["greeks"],
        bond_analytics=result.get("bond_analytics"),
        diagnostics=result.get("diagnostics", ""),
        mc_std_error=result.get("mc_std_error", 0.0),
    )


def price_vanilla(req: VanillaRequest) -> PricingResponse:
    if req.is_american:
        # Price American vanilla
        input_data = qm.AmericanVanillaBSInput()
        input_data.spot = req.spot
        input_data.strike = req.strike
        input_data.maturity = req.maturity
        input_data.rate = req.rate
        input_data.dividend = req.dividend
        input_data.vol = req.vol
        input_data.is_call = req.is_call
        input_data.tree_steps = req.tree_steps
        input_data.pde_space_steps = 100
        input_data.pde_time_steps = 100

        if req.engine == EngineType.trinomial:
            result = qm.price_american_vanilla_bs_trinomial(input_data)
        else:  # default to binomial for American
            result = qm.price_american_vanilla_bs_binomial(input_data)
    else:
        # Price European vanilla
        input_data = qm.VanillaBSInput()
        input_data.spot = req.spot
        input_data.strike = req.strike
        input_data.maturity = req.maturity
        input_data.rate = req.rate
        input_data.dividend = req.dividend
        input_data.vol = req.vol
        input_data.is_call = req.is_call
        input_data.n_paths = req.n_paths
        input_data.seed = req.seed
        input_data.mc_epsilon = req.mc_epsilon

        if req.engine == EngineType.pde:
            input_data.pde_space_steps = req.pde_space_steps
            input_data.pde_time_steps = req.pde_time_steps
            result = qm.price_vanilla_bs_pde(input_data)
        elif req.engine == EngineType.trinomial:
            input_data.tree_steps = req.tree_steps
            result = qm.price_vanilla_bs_trinomial(input_data)
        elif req.engine == EngineType.binomial:
            input_data.tree_steps = req.tree_steps
            result = qm.price_vanilla_bs_binomial(input_data)
        elif req.engine == EngineType.mc:
            result = qm.price_vanilla_bs_mc(input_data)
        else:
            result = qm.price_vanilla_bs_analytic(input_data)

    return _pricing_response_from_dict(result)


def price_american_vanilla(req: AmericanVanillaRequest) -> PricingResponse:
    input_data = qm.AmericanVanillaBSInput()
    input_data.spot = req.spot
    input_data.strike = req.strike
    input_data.maturity = req.maturity
    input_data.rate = req.rate
    input_data.dividend = req.dividend
    input_data.vol = req.vol
    input_data.is_call = req.is_call
    input_data.tree_steps = req.tree_steps
    input_data.pde_space_steps = req.pde_space_steps
    input_data.pde_time_steps = req.pde_time_steps

    if req.engine == AmericanEngineType.trinomial:
        result = qm.price_american_vanilla_bs_trinomial(input_data)
    else:  # default to binomial
        result = qm.price_american_vanilla_bs_binomial(input_data)

    return _pricing_response_from_dict(result)


def price_asian(req: AsianRequest) -> PricingResponse:
    input_data = qm.AsianBSInput()
    input_data.spot = req.spot
    input_data.strike = req.strike
    input_data.maturity = req.maturity
    input_data.rate = req.rate
    input_data.dividend = req.dividend
    input_data.vol = req.vol
    input_data.is_call = req.is_call
    input_data.n_paths = req.n_paths
    input_data.seed = req.seed
    input_data.mc_epsilon = req.mc_epsilon

    if req.average_type == AsianAverageType.geometric:
        input_data.average_type = qm.AsianAverageType.Geometric
    else:
        input_data.average_type = qm.AsianAverageType.Arithmetic

    if req.engine == EngineType.mc:
        result = qm.price_asian_bs_mc(input_data)
    else:
        result = qm.price_asian_bs_analytic(input_data)

    return _pricing_response_from_dict(result)


_BARRIER_KIND_MAP = {
    BarrierKind.up_and_in:   qm.BarrierType.UpAndIn,
    BarrierKind.up_and_out:  qm.BarrierType.UpAndOut,
    BarrierKind.down_and_in: qm.BarrierType.DownAndIn,
    BarrierKind.down_and_out: qm.BarrierType.DownAndOut,
}


def price_barrier(req: BarrierRequest) -> PricingResponse:
    input_data = qm.BarrierBSInput()
    input_data.spot = req.spot
    input_data.strike = req.strike
    input_data.maturity = req.maturity
    input_data.rate = req.rate
    input_data.dividend = req.dividend
    input_data.vol = req.vol
    input_data.is_call = req.is_call
    input_data.barrier_level = req.barrier_level
    input_data.barrier_type = _BARRIER_KIND_MAP[req.barrier_kind]
    input_data.rebate = req.rebate
    input_data.n_paths = req.n_paths
    input_data.seed = req.seed
    input_data.mc_epsilon = req.mc_epsilon
    input_data.n_steps = req.n_steps
    input_data.brownian_bridge = req.brownian_bridge
    result = qm.price_barrier_bs_mc(input_data)
    return _pricing_response_from_dict(result)


_DIGITAL_PAYOFF_MAP = {
    DigitalPayoffKind.cash_or_nothing:  qm.DigitalPayoffType.CashOrNothing,
    DigitalPayoffKind.asset_or_nothing: qm.DigitalPayoffType.AssetOrNothing,
}


def price_digital(req: DigitalRequest) -> PricingResponse:
    input_data = qm.DigitalBSInput()
    input_data.spot = req.spot
    input_data.strike = req.strike
    input_data.maturity = req.maturity
    input_data.rate = req.rate
    input_data.dividend = req.dividend
    input_data.vol = req.vol
    input_data.is_call = req.is_call
    input_data.payoff_type = _DIGITAL_PAYOFF_MAP[req.payoff_type]
    input_data.cash_amount = req.cash_amount
    result = qm.price_digital_bs_analytic(input_data)
    return _pricing_response_from_dict(result)


_LOOKBACK_STYLE_MAP = {
    LookbackStyle.fixed_strike:    qm.LookbackStyle.FixedStrike,
    LookbackStyle.floating_strike: qm.LookbackStyle.FloatingStrike,
}

_LOOKBACK_EXTREMUM_MAP = {
    LookbackExtremum.minimum: qm.LookbackExtremum.Minimum,
    LookbackExtremum.maximum: qm.LookbackExtremum.Maximum,
}


def price_lookback(req: LookbackRequest) -> PricingResponse:
    input_data = qm.LookbackBSInput()
    input_data.spot = req.spot
    input_data.strike = req.strike
    input_data.maturity = req.maturity
    input_data.rate = req.rate
    input_data.dividend = req.dividend
    input_data.vol = req.vol
    input_data.is_call = req.is_call
    input_data.style = _LOOKBACK_STYLE_MAP[req.style]
    input_data.extremum = _LOOKBACK_EXTREMUM_MAP[req.extremum]
    input_data.n_steps = req.n_steps
    input_data.n_paths = req.n_paths
    input_data.seed = req.seed
    input_data.mc_antithetic = req.mc_antithetic
    input_data.mc_epsilon = req.mc_epsilon
    result = qm.price_lookback_bs_mc(input_data)
    return _pricing_response_from_dict(result)


def price_basket(req: BasketRequest) -> PricingResponse:
    n = len(req.spots)
    # Per-asset dividends: use provided list or replicate a single 0.0
    dividends = list(req.dividends) if len(req.dividends) == n else [0.0] * n
    # Per-asset weights: use provided list or equal-weight
    weights = list(req.weights) if len(req.weights) == n else [1.0 / n] * n
    # Build full n×n correlation matrix from pairwise scalar
    rho = req.pairwise_correlation
    correlations = [
        [1.0 if i == j else rho for j in range(n)]
        for i in range(n)
    ]

    input_data = qm.BasketBSInput()
    input_data.spots        = req.spots
    input_data.vols         = req.vols
    input_data.dividends    = dividends
    input_data.weights      = weights
    input_data.correlations = correlations
    input_data.strike       = req.strike
    input_data.maturity     = req.maturity
    input_data.rate         = req.rate
    input_data.is_call      = req.is_call
    input_data.n_paths      = req.n_paths
    input_data.seed         = req.seed
    input_data.mc_antithetic = req.mc_antithetic

    result = qm.price_basket_bs_mc(input_data)
    return _pricing_response_from_dict(result)


def price_future(req: FutureRequest) -> PricingResponse:
    input_data = qm.EquityFutureInput()
    input_data.spot = req.spot
    input_data.strike = req.strike
    input_data.maturity = req.maturity
    input_data.rate = req.rate
    input_data.dividend = req.dividend
    input_data.notional = req.notional

    result = qm.price_future_bs_analytic(input_data)
    return _pricing_response_from_dict(result)


def price_zero_coupon_bond(req: ZeroCouponBondRequest) -> PricingResponse:
    input_data = qm.ZeroCouponBondInput()
    input_data.maturity = req.maturity
    input_data.rate = req.rate
    input_data.notional = req.notional
    input_data.discount_times = req.discount_times
    input_data.discount_factors = req.discount_factors

    result = qm.price_zero_coupon_bond_analytic(input_data)
    return _pricing_response_from_dict(result)


def price_fixed_rate_bond(req: FixedRateBondRequest) -> PricingResponse:
    input_data = qm.FixedRateBondInput()
    input_data.maturity = req.maturity
    input_data.rate = req.rate
    input_data.coupon_rate = req.coupon_rate
    input_data.coupon_frequency = req.coupon_frequency
    input_data.notional = req.notional
    input_data.discount_times = req.discount_times
    input_data.discount_factors = req.discount_factors

    result = qm.price_fixed_rate_bond_analytic(input_data)
    return _pricing_response_from_dict(result)


# ---------------------------------------------------------------------------
# Autocall
# ---------------------------------------------------------------------------

def price_autocall(req: AutocallRequest) -> PricingResponse:
    input_data = qm.AutocallBSInput()
    input_data.spot = req.spot
    input_data.rate = req.rate
    input_data.dividend = req.dividend
    input_data.vol = req.vol
    input_data.observation_dates = req.observation_dates
    input_data.autocall_barrier = req.autocall_barrier
    input_data.coupon_barrier = req.coupon_barrier
    input_data.put_barrier = req.put_barrier
    input_data.coupon_rate = req.coupon_rate
    input_data.notional = req.notional
    input_data.memory_coupon = req.memory_coupon
    input_data.ki_continuous = req.ki_continuous
    input_data.n_paths = req.n_paths
    input_data.seed = req.seed
    result = qm.price_autocall_bs_mc(input_data)
    return _pricing_response_from_dict(result)


# ---------------------------------------------------------------------------
# Mountain (Himalaya)
# ---------------------------------------------------------------------------

def _build_corr_matrix(n: int, corrs: List[List[float]]) -> List[List[float]]:
    """Return provided correlation matrix or identity if empty."""
    if corrs and len(corrs) == n:
        return corrs
    return [[1.0 if i == j else 0.0 for j in range(n)] for i in range(n)]


def price_mountain(req: MountainRequest) -> PricingResponse:
    n = len(req.spots)
    input_data = qm.MountainBSInput()
    input_data.spots = req.spots
    input_data.vols = req.vols
    input_data.dividends = list(req.dividends) if len(req.dividends) == n else [0.0] * n
    input_data.correlations = _build_corr_matrix(n, req.correlations)
    input_data.observation_dates = req.observation_dates
    input_data.strike = req.strike
    input_data.is_call = req.is_call
    input_data.rate = req.rate
    input_data.notional = req.notional
    input_data.n_paths = req.n_paths
    input_data.seed = req.seed
    result = qm.price_mountain_bs_mc(input_data)
    return _pricing_response_from_dict(result)


# ---------------------------------------------------------------------------
# Variance Swap
# ---------------------------------------------------------------------------

def price_variance_swap(req: VarianceSwapRequest) -> PricingResponse:
    input_data = qm.VarianceSwapBSInput()
    input_data.spot = req.spot
    input_data.rate = req.rate
    input_data.dividend = req.dividend
    input_data.vol = req.vol
    input_data.maturity = req.maturity
    input_data.strike_var = req.strike_var
    input_data.notional = req.notional
    input_data.observation_dates = req.observation_dates
    input_data.n_paths = req.n_paths
    input_data.seed = req.seed
    if req.engine == EngineType.mc:
        result = qm.price_variance_swap_bs_mc(input_data)
    else:
        result = qm.price_variance_swap_bs_analytic(input_data)
    return _pricing_response_from_dict(result)


# ---------------------------------------------------------------------------
# Volatility Swap
# ---------------------------------------------------------------------------

def price_volatility_swap(req: VolatilitySwapRequest) -> PricingResponse:
    input_data = qm.VolatilitySwapBSInput()
    input_data.spot = req.spot
    input_data.rate = req.rate
    input_data.dividend = req.dividend
    input_data.vol = req.vol
    input_data.maturity = req.maturity
    input_data.strike_vol = req.strike_vol
    input_data.notional = req.notional
    input_data.observation_dates = req.observation_dates
    input_data.n_paths = req.n_paths
    input_data.seed = req.seed
    result = qm.price_volatility_swap_bs_mc(input_data)
    return _pricing_response_from_dict(result)


# ---------------------------------------------------------------------------
# Dispersion Swap
# ---------------------------------------------------------------------------

def price_dispersion_swap(req: DispersionSwapRequest) -> PricingResponse:
    n = len(req.spots)
    dividends = list(req.dividends) if len(req.dividends) == n else [0.0] * n
    weights = list(req.weights) if len(req.weights) == n else [1.0 / n] * n
    rho = req.pairwise_correlation
    correlations = [[1.0 if i == j else rho for j in range(n)] for i in range(n)]

    input_data = qm.DispersionBSInput()
    input_data.spots = req.spots
    input_data.vols = req.vols
    input_data.dividends = dividends
    input_data.weights = weights
    input_data.correlations = correlations
    input_data.maturity = req.maturity
    input_data.strike_spread = req.strike_spread
    input_data.rate = req.rate
    input_data.notional = req.notional
    input_data.observation_dates = req.observation_dates
    input_data.n_paths = req.n_paths
    input_data.seed = req.seed
    result = qm.price_dispersion_bs_mc(input_data)
    return _pricing_response_from_dict(result)


# ---------------------------------------------------------------------------
# FX Forward
# ---------------------------------------------------------------------------

def price_fx_forward(req: FXForwardRequest) -> PricingResponse:
    input_data = qm.FXForwardInput()
    input_data.spot = req.spot
    input_data.rate_domestic = req.rate_domestic
    input_data.rate_foreign = req.rate_foreign
    input_data.vol = req.vol
    input_data.strike = req.strike
    input_data.maturity = req.maturity
    input_data.notional = req.notional
    result = qm.price_fx_forward_analytic(input_data)
    return _pricing_response_from_dict(result)


# ---------------------------------------------------------------------------
# FX Option
# ---------------------------------------------------------------------------

def price_fx_option(req: FXOptionRequest) -> PricingResponse:
    input_data = qm.FXOptionInput()
    input_data.spot = req.spot
    input_data.rate_domestic = req.rate_domestic
    input_data.rate_foreign = req.rate_foreign
    input_data.vol = req.vol
    input_data.strike = req.strike
    input_data.maturity = req.maturity
    input_data.is_call = req.is_call
    input_data.notional = req.notional
    result = qm.price_fx_option_analytic(input_data)
    return _pricing_response_from_dict(result)


# ---------------------------------------------------------------------------
# Commodity Forward
# ---------------------------------------------------------------------------

def price_commodity_forward(req: CommodityForwardRequest) -> PricingResponse:
    input_data = qm.CommodityForwardInput()
    input_data.spot = req.spot
    input_data.rate = req.rate
    input_data.storage_cost = req.storage_cost
    input_data.convenience_yield = req.convenience_yield
    input_data.vol = req.vol
    input_data.strike = req.strike
    input_data.maturity = req.maturity
    input_data.notional = req.notional
    result = qm.price_commodity_forward_analytic(input_data)
    return _pricing_response_from_dict(result)


# ---------------------------------------------------------------------------
# Commodity Option
# ---------------------------------------------------------------------------

def price_commodity_option(req: CommodityOptionRequest) -> PricingResponse:
    input_data = qm.CommodityOptionInput()
    input_data.spot = req.spot
    input_data.rate = req.rate
    input_data.storage_cost = req.storage_cost
    input_data.convenience_yield = req.convenience_yield
    input_data.vol = req.vol
    input_data.strike = req.strike
    input_data.maturity = req.maturity
    input_data.is_call = req.is_call
    input_data.notional = req.notional
    result = qm.price_commodity_option_analytic(input_data)
    return _pricing_response_from_dict(result)


# ---------------------------------------------------------------------------
# Rainbow (worst-of / best-of)
# ---------------------------------------------------------------------------

def price_rainbow(req: RainbowRequest) -> PricingResponse:
    n = len(req.spots)
    dividends = list(req.dividends) if len(req.dividends) == n else [0.0] * n
    rho = req.pairwise_correlation
    correlations = [[1.0 if i == j else rho for j in range(n)] for i in range(n)]

    input_data = qm.RainbowBSInput()
    input_data.spots = req.spots
    input_data.vols = req.vols
    input_data.dividends = dividends
    input_data.correlations = correlations
    input_data.maturity = req.maturity
    input_data.strike = req.strike
    input_data.is_call = req.is_call
    input_data.rate = req.rate
    input_data.notional = req.notional
    input_data.n_paths = req.n_paths
    input_data.seed = req.seed

    if req.rainbow_kind == RainbowKind.best_of:
        result = qm.price_best_of_bs_mc(input_data)
    else:
        result = qm.price_worst_of_bs_mc(input_data)
    return _pricing_response_from_dict(result)


if __name__ == "__main__":
    print("Running binding smoke tests...")

    vanilla = qm.VanillaBSInput()
    vanilla.spot = 100.0
    vanilla.strike = 100.0
    vanilla.maturity = 1.0
    vanilla.rate = 0.05
    vanilla.dividend = 0.02
    vanilla.vol = 0.2
    vanilla.is_call = True
    vanilla.n_paths = 200000
    vanilla.seed = 42
    vanilla.mc_epsilon = 0.0
    print("vanilla_analytic:", qm.price_vanilla_bs_analytic(vanilla))
    
    american = qm.AmericanVanillaBSInput()
    american.spot = 100.0
    american.strike = 100.0
    american.maturity = 1.0
    american.rate = 0.05
    american.dividend = 0.02
    american.vol = 0.2
    american.is_call = False
    american.tree_steps = 100
    print("american_put_binomial:", qm.price_american_vanilla_bs_binomial(american))
    print("american_put_trinomial:", qm.price_american_vanilla_bs_trinomial(american))

    zc = qm.ZeroCouponBondInput()
    zc.maturity = 2.0
    zc.rate = 0.03
    zc.notional = 1000.0
    zc.discount_times = [0.5, 1.0, 2.0, 3.0]
    zc.discount_factors = [0.985, 0.97, 0.94, 0.915]
    print("zero_coupon:", qm.price_zero_coupon_bond_analytic(zc))

    fixed = qm.FixedRateBondInput()
    fixed.maturity = 3.0
    fixed.rate = 0.032
    fixed.coupon_rate = 0.045
    fixed.coupon_frequency = 2
    fixed.notional = 1000.0
    fixed.discount_times = [0.5, 1.0, 2.0, 3.0, 5.0]
    fixed.discount_factors = [0.988, 0.975, 0.945, 0.92, 0.885]
    print("fixed_rate:", qm.price_fixed_rate_bond_analytic(fixed))