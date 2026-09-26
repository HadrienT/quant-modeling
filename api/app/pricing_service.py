from datetime import datetime, timezone
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
    ComputeDevice,
    CommodityForwardRequest,
    CommodityOptionRequest,
    DatedAsianRequest,
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
    McRng,
    ModelChoice,
    MountainRequest,
    PricingResponse,
    QuantoRequest,
    RainbowKind,
    RainbowRequest,
    ScriptRequest,
    ScriptValidateRequest,
    ScriptValidateResponse,
    ScriptedProductRequest,
    UnderlyingUsed,
    VanillaRequest,
    VarianceSwapRequest,
    VolatilitySwapRequest,
    ZeroCouponBondRequest,
)
from .telemetry import tracer


def _pricing_response_from_dict(result: Dict) -> PricingResponse:
    return PricingResponse(
        npv=result["npv"],
        greeks=result["greeks"],
        bond_analytics=result.get("bond_analytics"),
        diagnostics=result.get("diagnostics", ""),
        mc_std_error=result.get("mc_std_error", 0.0),
        device=result.get("device", "cpu"),
        # Only price_script(greeks_method="aad") populates this today
        # (blueprint/wp/17-aad.md §13.1); every other pricer's dict carries
        # risks=None, same as before this field existed.
        risks=result.get("risks"),
        # Only price_script populates this (scripting/model_advice.hpp).
        warnings=result.get("warnings", []),
    )


_DEVICES = {
    ComputeDevice.cpu: qm.ComputeDevice.Cpu,
    ComputeDevice.gpu: qm.ComputeDevice.Gpu,
    ComputeDevice.auto: qm.ComputeDevice.Auto,
}
_RNGS = {McRng.pcg32: qm.RngKind.Pcg32, McRng.philox: qm.RngKind.Philox}


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
        input_data.device = _DEVICES[req.device]
        input_data.rng = _RNGS[req.rng]

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


def price_dated_asian(req: DatedAsianRequest) -> PricingResponse:
    result = qm.price_dated_asian(
        req.spot,
        req.rate,
        req.dividend,
        req.vol,
        req.valuation_date.isoformat(),
        [d.isoformat() for d in req.fixing_dates],
        req.strike,
        req.is_call,
        req.geometric,
        req.day_count,
        req.n_paths,
        req.seed,
        req.sampler,
    )
    return _pricing_response_from_dict(result)


def validate_script(req: ScriptValidateRequest) -> ScriptValidateResponse:
    result = qm.validate_script(
        req.script, req.valuation_date.isoformat(), req.day_count
    )
    return ScriptValidateResponse(**result)


def _user_choice(model: str) -> Dict:
    return {
        "requested": model,
        "model": model,
        "code": "user",
        "reason": "Chosen by hand.",
    }


def _calibration_of(market, sv, model: str) -> Dict:
    """What was calibrated for `model`, for the response."""
    out: Dict = {"ticker": market.ticker, "snapshot": market.valuation_date}
    if sv is None:
        out["seconds"] = 0.0
        return out
    out["seconds"] = sv.seconds
    out["heston"] = {
        **sv.heston,
        "iv_rmse": sv.iv_rmse,
        "iv_worst": sv.iv_worst,
        "n_quotes": sv.n_quotes,
        "n_maturities": sv.n_maturities,
        "feller": sv.feller,
    }
    if model == "slv":
        out["leverage"] = {
            "min": sv.leverage_min,
            "max": sv.leverage_max,
            "clamped_share": sv.leverage_clamped_share,
            "n_particles": sv.n_particles,
        }
    return out


def _price_multi_asset(req: ScriptRequest) -> PricingResponse:
    """A script reading spot(0), spot(1)...: correlated Black-Scholes, inputs
    from the database (tickers) or typed."""
    from . import multi_asset_market

    parsed = qm.validate_script(
        req.script, req.valuation_date.isoformat(), req.day_count
    )
    n, given = parsed["analysis"]["n_underlyings"], len(req.underlyings)
    if n != given:
        raise ValueError(
            f"the script reads {n} underlying(s) (spot(0) to spot({n - 1})) "
            f"but {given} are given"
        )
    horizon = parsed["events"][-1]["t"] if parsed["events"] else 0.0
    warnings: List[Dict] = []
    if req.underlyings[0].ticker is not None:
        m = multi_asset_market.multi_asset_market(
            [u.ticker for u in req.underlyings],
            req.rate,
            req.valuation_date,
            horizon,
        )
        used = [
            UnderlyingUsed(
                ticker=a.ticker,
                spot=a.spot,
                dividend=a.dividend,
                vol=a.vol,
                vol_source=a.vol_source,
            )
            for a in m.assets
        ]
        corr, corr_source, warnings = m.correlation, m.correlation_source, m.warnings
    else:
        used = [
            UnderlyingUsed(
                spot=u.spot, dividend=u.dividend, vol=u.vol, vol_source="typed"
            )
            for u in req.underlyings
        ]
        corr, corr_source = req.correlation, "typed"

    rec = qm.recommend_script_model(
        req.script,
        req.valuation_date.isoformat(),
        req.day_count,
        market_surface=False,
        stochastic=False,
    )
    choice = (
        {"requested": "auto", **rec}
        if req.model == "auto"
        else _user_choice("black_scholes")
    )
    with tracer.start_as_current_span(
        "engine.price",
        attributes={
            "qm.product": "script",
            "qm.model": "black_scholes",
            "qm.engine": "mc",
        },
    ):
        result = qm.price_script(
            req.script,
            used[0].spot,
            req.rate,
            used[0].dividend,
            used[0].vol,
            req.valuation_date.isoformat(),
            req.day_count,
            req.fuzzy,
            req.default_eps,
            req.n_paths,
            req.seed,
            req.sampler,
            req.greeks_method,
            "black_scholes",
            [],
            [],
            [],
            req.steps_per_year,
            spots=[u.spot for u in used],
            dividends=[u.dividend for u in used],
            vols=[u.vol for u in used],
            correlation=[x for row in corr for x in row],
            device=req.device.value,
            rng=req.rng.value,
        )
    result["warnings"] = warnings + list(result.get("warnings", []))
    response = _pricing_response_from_dict(result)
    return response.model_copy(
        update={
            "model_choice": ModelChoice(
                **choice,
                underlyings=used,
                correlation=corr,
                correlation_source=corr_source,
            )
        }
    )


def price_scripted_product(req: ScriptedProductRequest) -> PricingResponse:
    """A library product from its term sheet (product_templates.py): the
    script is rendered with the terms, then priced like any script."""
    from . import product_templates

    script = product_templates.render(req.product, req.terms, req.valuation_date)
    fields = req.model_dump(exclude={"product", "terms"})
    response = price_script(ScriptRequest(script=script, **fields))
    return response.model_copy(update={"script": script})


def price_script(req: ScriptRequest) -> PricingResponse:
    """Choose the model (req.model, or the one the script needs for 'auto'),
    calibrate what it needs from the database, then price."""
    from . import market_snapshot, stochastic_vol

    if req.underlyings is not None:
        return _price_multi_asset(req)

    market_warnings: List[Dict] = []
    valuation_date = req.valuation_date
    market = None
    if req.ticker and (req.model != "black_scholes" or req.spot is None):
        # Market data comes from the database data-ingest fills, never from a
        # live source. A stored snapshot is a market date, and that date is
        # the valuation date the script is priced on.
        market = market_snapshot.local_vol_market(
            req.ticker, req.rate, req.valuation_date
        )
        valuation_date = market.valuation_date
        market_warnings = list(market.warnings)

    model = req.model
    if model == "auto":
        choice = {"requested": "auto", **_recommend(req, valuation_date, market, True)}
    else:
        choice = _user_choice(model)

    sv = None
    if choice["model"] in ("heston", "slv"):
        try:
            sv = stochastic_vol.calibrate(market)
        except stochastic_vol.CalibrationUnavailable as exc:
            if req.model != "auto":
                raise ValueError(
                    f"model='{req.model}' cannot be calibrated: {exc}"
                ) from exc
            # 'auto' falls back to the best model still available, and says so.
            choice = {
                "requested": "auto",
                **_recommend(req, valuation_date, market, False),
            }
            market_warnings.append(
                {
                    "code": "stochastic_calibration_failed",
                    "severity": "warning",
                    "message": f"Stochastic-vol calibration failed ({exc}); "
                    f"priced under {choice['model']} instead.",
                }
            )
    model = choice["model"]
    if market is not None:
        choice["calibration"] = _calibration_of(market, sv, model)

    if market is not None:
        spot, dividend, vol = market.spot, market.dividend, 0.0
        k_grid, t_grid = market.K_grid, market.T_grid
        if model == "black_scholes":
            vol = _atm_implied_vol(req, market, valuation_date)
            choice["calibration"]["flat_vol"] = vol
    else:
        spot, dividend, vol = req.spot, req.dividend, req.vol
        k_grid, t_grid = [], []
    sigma = market.sigma_loc_flat if model == "local_vol" else []
    heston = sv.heston if sv is not None else {}
    leverage = list(sv.leverage_flat) if (sv is not None and model == "slv") else []

    with tracer.start_as_current_span(
        "engine.price",
        attributes={"qm.product": "script", "qm.model": model, "qm.engine": "mc"},
    ):
        result = qm.price_script(
            req.script,
            spot,
            req.rate,
            dividend,
            vol,
            valuation_date.isoformat(),
            req.day_count,
            req.fuzzy,
            req.default_eps,
            req.n_paths,
            req.seed,
            req.sampler,
            req.greeks_method,
            model,
            k_grid if model in ("local_vol", "slv") else [],
            t_grid if model in ("local_vol", "slv") else [],
            sigma,
            req.steps_per_year,
            heston=heston,
            leverage_flat=leverage,
            device=req.device.value,
            rng=req.rng.value,
        )
    result["warnings"] = market_warnings + list(result.get("warnings", []))
    response = _pricing_response_from_dict(result)
    return response.model_copy(update={"model_choice": ModelChoice(**choice)})


def _atm_implied_vol(req, market, valuation_date) -> float:
    """Flat Black-Scholes on a ticker: the at-the-money implied vol of the
    stored SVI smile at the script's last event (sticky strike, K = spot)."""
    from . import vol_smile
    from .audit.payloads import MarketInputStatus
    from .valuation import record_market_input

    parsed = qm.validate_script(req.script, valuation_date.isoformat(), req.day_count)
    horizon = parsed["events"][-1]["t"] if parsed["events"] else 0.0
    smile = vol_smile.Smile(
        ticker=market.ticker,
        snapshot=market.valuation_date,
        spot=market.spot,
        rate=market.rate,
        dividend=market.dividend,
        slices=tuple(sorted(market.svi_slices, key=lambda s: s["ttm"])),
        K_grid=(),
        T_grid=(),
        sigma_loc_flat=(),
    )
    vol = smile.implied_vol(market.spot, max(horizon, 1e-4))
    record_market_input(
        f"vol:{market.ticker}",
        "db:options.chain_snapshot",
        market.valuation_date.isoformat(),
        MarketInputStatus.OBSERVED,
        vol,
    )
    return vol


def _recommend(req: ScriptRequest, valuation_date, market, stochastic: bool) -> Dict:
    return qm.recommend_script_model(
        req.script,
        valuation_date.isoformat(),
        req.day_count,
        market_surface=market is not None,
        stochastic=stochastic,
    )


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

def price_quanto(req: QuantoRequest) -> PricingResponse:
    input_data = qm.QuantoBSInput()
    for field in (
        "spot", "strike", "maturity", "rate_domestic", "rate_foreign", "dividend",
        "vol", "fx_vol", "correlation", "fx_rate", "is_call", "n_paths", "seed",
    ):  # fmt: skip
        setattr(input_data, field, getattr(req, field))
    if req.engine == "mc":
        result = qm.price_quanto_bs_mc(input_data)
    else:
        result = qm.price_quanto_bs_analytic(input_data)
    return _pricing_response_from_dict(result)


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