import asyncio
from typing import Callable, TypeVar

from fastapi import APIRouter, HTTPException
from starlette.concurrency import run_in_threadpool

from ..logging_utils import get_logger
from ..pricing_service import (
    price_american_vanilla,
    price_asian,
    price_autocall,
    price_barrier,
    price_basket,
    price_commodity_forward,
    price_commodity_option,
    price_digital,
    price_dispersion_swap,
    price_fixed_rate_bond,
    price_future,
    price_fx_forward,
    price_fx_option,
    price_lookback,
    price_mountain,
    price_rainbow,
    price_vanilla,
    price_variance_swap,
    price_volatility_swap,
    price_zero_coupon_bond,
)
from ..schemas import (
    AmericanVanillaRequest,
    AsianRequest,
    AutocallRequest,
    BarrierRequest,
    BasketRequest,
    CommodityForwardRequest,
    CommodityOptionRequest,
    DigitalRequest,
    DispersionSwapRequest,
    FixedRateBondRequest,
    FutureRequest,
    FXForwardRequest,
    FXOptionRequest,
    LookbackRequest,
    MountainRequest,
    PricingResponse,
    RainbowRequest,
    VanillaRequest,
    VarianceSwapRequest,
    VolatilitySwapRequest,
    ZeroCouponBondRequest,
)

router = APIRouter()
logger = get_logger()
_PRICING_TIMEOUT_SECONDS = 20
_T = TypeVar("_T")


async def _run_with_timeout(func: Callable[..., _T], *args) -> _T:
    try:
        async with asyncio.timeout(_PRICING_TIMEOUT_SECONDS):
            return await run_in_threadpool(func, *args)
    except TimeoutError as exc:
        raise HTTPException(
            status_code=408,
            detail=f"Pricing timed out after {_PRICING_TIMEOUT_SECONDS} seconds. Reduce paths or grid steps and retry.",
        ) from exc


@router.post("/price/option/vanilla", response_model=PricingResponse)
async def price_vanilla_endpoint(req: VanillaRequest) -> PricingResponse:
    logger.info(
        "price_vanilla request",
        extra={
            "spot": req.spot,
            "strike": req.strike,
            "maturity": req.maturity,
            "rate": req.rate,
            "dividend": req.dividend,
            "vol": req.vol,
            "is_call": req.is_call,
            "is_american": req.is_american,
            "engine": req.engine,
            "n_paths": req.n_paths,
            "seed": req.seed,
            "mc_epsilon": req.mc_epsilon,
            "tree_steps": req.tree_steps,
        },
    )
    resp = await _run_with_timeout(price_vanilla, req)
    logger.info(
        "price_vanilla response",
        extra={
            "npv": resp.npv,
            "mc_std_error": resp.mc_std_error,
            "diagnostics": resp.diagnostics,
            "delta": resp.greeks.delta,
            "gamma": resp.greeks.gamma,
            "vega": resp.greeks.vega,
            "theta": resp.greeks.theta,
            "rho": resp.greeks.rho,
        },
    )
    return resp


@router.post("/price/option/american-vanilla", response_model=PricingResponse)
async def price_american_vanilla_endpoint(req: AmericanVanillaRequest) -> PricingResponse:
    logger.info(
        "price_american_vanilla request",
        extra={
            "spot": req.spot,
            "strike": req.strike,
            "maturity": req.maturity,
            "rate": req.rate,
            "dividend": req.dividend,
            "vol": req.vol,
            "is_call": req.is_call,
            "engine": req.engine,
            "tree_steps": req.tree_steps,
        },
    )
    resp = await _run_with_timeout(price_american_vanilla, req)
    logger.info(
        "price_american_vanilla response",
        extra={
            "npv": resp.npv,
            "diagnostics": resp.diagnostics,
            "delta": resp.greeks.delta,
            "gamma": resp.greeks.gamma,
            "vega": resp.greeks.vega,
            "theta": resp.greeks.theta,
            "rho": resp.greeks.rho,
        },
    )
    return resp


@router.post("/price/option/asian", response_model=PricingResponse)
async def price_asian_endpoint(req: AsianRequest) -> PricingResponse:
    logger.info(
        "price_asian request",
        extra={
            "spot": req.spot,
            "strike": req.strike,
            "maturity": req.maturity,
            "rate": req.rate,
            "dividend": req.dividend,
            "vol": req.vol,
            "is_call": req.is_call,
            "average_type": req.average_type,
            "engine": req.engine,
            "n_paths": req.n_paths,
            "seed": req.seed,
            "mc_epsilon": req.mc_epsilon,
        },
    )
    resp = await _run_with_timeout(price_asian, req)
    logger.info(
        "price_asian response",
        extra={
            "npv": resp.npv,
            "mc_std_error": resp.mc_std_error,
            "diagnostics": resp.diagnostics,
            "delta": resp.greeks.delta,
            "gamma": resp.greeks.gamma,
            "vega": resp.greeks.vega,
            "theta": resp.greeks.theta,
            "rho": resp.greeks.rho,
        },
    )
    return resp


@router.post("/price/option/barrier", response_model=PricingResponse)
async def price_barrier_endpoint(req: BarrierRequest) -> PricingResponse:
    logger.info(
        "price_barrier request",
        extra={
            "spot": req.spot,
            "strike": req.strike,
            "maturity": req.maturity,
            "rate": req.rate,
            "dividend": req.dividend,
            "vol": req.vol,
            "is_call": req.is_call,
            "barrier_kind": req.barrier_kind,
            "barrier_level": req.barrier_level,
            "rebate": req.rebate,
            "n_paths": req.n_paths,
            "seed": req.seed,
            "mc_epsilon": req.mc_epsilon,
        },
    )
    resp = await _run_with_timeout(price_barrier, req)
    logger.info(
        "price_barrier response",
        extra={
            "npv": resp.npv,
            "mc_std_error": resp.mc_std_error,
            "diagnostics": resp.diagnostics,
            "delta": resp.greeks.delta,
            "gamma": resp.greeks.gamma,
            "vega": resp.greeks.vega,
            "theta": resp.greeks.theta,
            "rho": resp.greeks.rho,
        },
    )
    return resp


@router.post("/price/option/digital", response_model=PricingResponse)
async def price_digital_endpoint(req: DigitalRequest) -> PricingResponse:
    logger.info(
        "price_digital request",
        extra={
            "spot": req.spot,
            "strike": req.strike,
            "maturity": req.maturity,
            "rate": req.rate,
            "dividend": req.dividend,
            "vol": req.vol,
            "is_call": req.is_call,
            "payoff_type": req.payoff_type,
            "cash_amount": req.cash_amount,
        },
    )
    resp = await _run_with_timeout(price_digital, req)
    logger.info(
        "price_digital response",
        extra={
            "npv": resp.npv,
            "diagnostics": resp.diagnostics,
        },
    )
    return resp


@router.post("/price/option/lookback", response_model=PricingResponse)
async def price_lookback_endpoint(req: LookbackRequest) -> PricingResponse:
    logger.info(
        "price_lookback request",
        extra={
            "spot": req.spot,
            "strike": req.strike,
            "maturity": req.maturity,
            "rate": req.rate,
            "dividend": req.dividend,
            "vol": req.vol,
            "is_call": req.is_call,
            "style": req.style,
            "extremum": req.extremum,
            "n_steps": req.n_steps,
            "n_paths": req.n_paths,
            "seed": req.seed,
            "mc_antithetic": req.mc_antithetic,
        },
    )
    resp = await _run_with_timeout(price_lookback, req)
    logger.info(
        "price_lookback response",
        extra={
            "npv": resp.npv,
            "mc_std_error": resp.mc_std_error,
            "diagnostics": resp.diagnostics,
            "delta": resp.greeks.delta,
            "gamma": resp.greeks.gamma,
            "vega": resp.greeks.vega,
            "theta": resp.greeks.theta,
            "rho": resp.greeks.rho,
        },
    )
    return resp


@router.post("/price/option/basket", response_model=PricingResponse)
async def price_basket_endpoint(req: BasketRequest) -> PricingResponse:
    logger.info(
        "price_basket request",
        extra={
            "n_assets": len(req.spots),
            "spots": req.spots,
            "vols": req.vols,
            "weights": req.weights,
            "pairwise_correlation": req.pairwise_correlation,
            "strike": req.strike,
            "maturity": req.maturity,
            "rate": req.rate,
            "is_call": req.is_call,
            "n_paths": req.n_paths,
            "seed": req.seed,
        },
    )
    resp = await _run_with_timeout(price_basket, req)
    logger.info(
        "price_basket response",
        extra={
            "npv": resp.npv,
            "mc_std_error": resp.mc_std_error,
            "diagnostics": resp.diagnostics,
            "delta": resp.greeks.delta,
            "gamma": resp.greeks.gamma,
            "vega": resp.greeks.vega,
            "theta": resp.greeks.theta,
            "rho": resp.greeks.rho,
        },
    )
    return resp


@router.post("/price/future", response_model=PricingResponse)
async def price_future_endpoint(req: FutureRequest) -> PricingResponse:
    logger.info(
        "price_future request",
        extra={
            "spot": req.spot,
            "strike": req.strike,
            "maturity": req.maturity,
            "rate": req.rate,
            "dividend": req.dividend,
            "notional": req.notional,
        },
    )
    resp = await _run_with_timeout(price_future, req)
    logger.info(
        "price_future response",
        extra={
            "npv": resp.npv,
            "mc_std_error": resp.mc_std_error,
            "diagnostics": resp.diagnostics,
        },
    )
    return resp


@router.post("/price/bond/zero-coupon", response_model=PricingResponse)
async def price_zero_coupon_bond_endpoint(req: ZeroCouponBondRequest) -> PricingResponse:
    logger.info(
        "price_zero_coupon_bond request",
        extra={
            "maturity": req.maturity,
            "rate": req.rate,
            "notional": req.notional,
            "discount_times": req.discount_times,
            "discount_factors": req.discount_factors,
        },
    )
    resp = await _run_with_timeout(price_zero_coupon_bond, req)
    logger.info(
        "price_zero_coupon_bond response",
        extra={
            "npv": resp.npv,
            "mc_std_error": resp.mc_std_error,
            "diagnostics": resp.diagnostics,
            "bond_analytics": resp.bond_analytics.model_dump() if resp.bond_analytics else None,
        },
    )
    return resp


@router.post("/price/bond/fixed-rate", response_model=PricingResponse)
async def price_fixed_rate_bond_endpoint(req: FixedRateBondRequest) -> PricingResponse:
    logger.info(
        "price_fixed_rate_bond request",
        extra={
            "maturity": req.maturity,
            "rate": req.rate,
            "coupon_rate": req.coupon_rate,
            "coupon_frequency": req.coupon_frequency,
            "notional": req.notional,
            "discount_times": req.discount_times,
            "discount_factors": req.discount_factors,
        },
    )
    resp = await _run_with_timeout(price_fixed_rate_bond, req)
    logger.info(
        "price_fixed_rate_bond response",
        extra={
            "npv": resp.npv,
            "mc_std_error": resp.mc_std_error,
            "diagnostics": resp.diagnostics,
            "bond_analytics": resp.bond_analytics.model_dump() if resp.bond_analytics else None,
        },
    )
    return resp


# ---------------------------------------------------------------------------
# Autocall
# ---------------------------------------------------------------------------

@router.post("/price/structured/autocall", response_model=PricingResponse)
async def price_autocall_endpoint(req: AutocallRequest) -> PricingResponse:
    logger.info("price_autocall request", extra={"spot": req.spot, "vol": req.vol, "n_obs": len(req.observation_dates)})
    resp = await _run_with_timeout(price_autocall, req)
    logger.info("price_autocall response", extra={"npv": resp.npv, "mc_std_error": resp.mc_std_error})
    return resp


# ---------------------------------------------------------------------------
# Mountain (Himalaya)
# ---------------------------------------------------------------------------

@router.post("/price/structured/mountain", response_model=PricingResponse)
async def price_mountain_endpoint(req: MountainRequest) -> PricingResponse:
    logger.info("price_mountain request", extra={"n_assets": len(req.spots), "n_obs": len(req.observation_dates)})
    resp = await _run_with_timeout(price_mountain, req)
    logger.info("price_mountain response", extra={"npv": resp.npv, "mc_std_error": resp.mc_std_error})
    return resp


# ---------------------------------------------------------------------------
# Variance Swap
# ---------------------------------------------------------------------------

@router.post("/price/volatility/variance-swap", response_model=PricingResponse)
async def price_variance_swap_endpoint(req: VarianceSwapRequest) -> PricingResponse:
    logger.info("price_variance_swap request", extra={"spot": req.spot, "strike_var": req.strike_var, "engine": req.engine})
    resp = await _run_with_timeout(price_variance_swap, req)
    logger.info("price_variance_swap response", extra={"npv": resp.npv, "mc_std_error": resp.mc_std_error})
    return resp


# ---------------------------------------------------------------------------
# Volatility Swap
# ---------------------------------------------------------------------------

@router.post("/price/volatility/volatility-swap", response_model=PricingResponse)
async def price_volatility_swap_endpoint(req: VolatilitySwapRequest) -> PricingResponse:
    logger.info("price_volatility_swap request", extra={"spot": req.spot, "strike_vol": req.strike_vol})
    resp = await _run_with_timeout(price_volatility_swap, req)
    logger.info("price_volatility_swap response", extra={"npv": resp.npv, "mc_std_error": resp.mc_std_error})
    return resp


# ---------------------------------------------------------------------------
# Dispersion Swap
# ---------------------------------------------------------------------------

@router.post("/price/volatility/dispersion-swap", response_model=PricingResponse)
async def price_dispersion_swap_endpoint(req: DispersionSwapRequest) -> PricingResponse:
    logger.info("price_dispersion_swap request", extra={"n_assets": len(req.spots), "strike_spread": req.strike_spread})
    resp = await _run_with_timeout(price_dispersion_swap, req)
    logger.info("price_dispersion_swap response", extra={"npv": resp.npv, "mc_std_error": resp.mc_std_error})
    return resp


# ---------------------------------------------------------------------------
# FX Forward
# ---------------------------------------------------------------------------

@router.post("/price/fx/forward", response_model=PricingResponse)
async def price_fx_forward_endpoint(req: FXForwardRequest) -> PricingResponse:
    logger.info("price_fx_forward request", extra={"spot": req.spot, "strike": req.strike, "maturity": req.maturity})
    resp = await _run_with_timeout(price_fx_forward, req)
    logger.info("price_fx_forward response", extra={"npv": resp.npv})
    return resp


# ---------------------------------------------------------------------------
# FX Option
# ---------------------------------------------------------------------------

@router.post("/price/fx/option", response_model=PricingResponse)
async def price_fx_option_endpoint(req: FXOptionRequest) -> PricingResponse:
    logger.info("price_fx_option request", extra={"spot": req.spot, "strike": req.strike, "is_call": req.is_call})
    resp = await _run_with_timeout(price_fx_option, req)
    logger.info("price_fx_option response", extra={"npv": resp.npv})
    return resp


# ---------------------------------------------------------------------------
# Commodity Forward
# ---------------------------------------------------------------------------

@router.post("/price/commodity/forward", response_model=PricingResponse)
async def price_commodity_forward_endpoint(req: CommodityForwardRequest) -> PricingResponse:
    logger.info("price_commodity_forward request", extra={"spot": req.spot, "strike": req.strike})
    resp = await _run_with_timeout(price_commodity_forward, req)
    logger.info("price_commodity_forward response", extra={"npv": resp.npv})
    return resp


# ---------------------------------------------------------------------------
# Commodity Option
# ---------------------------------------------------------------------------

@router.post("/price/commodity/option", response_model=PricingResponse)
async def price_commodity_option_endpoint(req: CommodityOptionRequest) -> PricingResponse:
    logger.info("price_commodity_option request", extra={"spot": req.spot, "strike": req.strike, "is_call": req.is_call})
    resp = await _run_with_timeout(price_commodity_option, req)
    logger.info("price_commodity_option response", extra={"npv": resp.npv})
    return resp


# ---------------------------------------------------------------------------
# Rainbow (worst-of / best-of)
# ---------------------------------------------------------------------------

@router.post("/price/option/rainbow", response_model=PricingResponse)
async def price_rainbow_endpoint(req: RainbowRequest) -> PricingResponse:
    logger.info("price_rainbow request", extra={"n_assets": len(req.spots), "kind": req.rainbow_kind, "is_call": req.is_call})
    resp = await _run_with_timeout(price_rainbow, req)
    logger.info("price_rainbow response", extra={"npv": resp.npv, "mc_std_error": resp.mc_std_error})
    return resp
