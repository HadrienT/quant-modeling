import asyncio
from typing import Callable, TypeVar

from fastapi import APIRouter, HTTPException
from starlette.concurrency import run_in_threadpool

from ..logging_utils import get_logger
from ..pricing_service import (
    price_american_vanilla,
    price_asian,
    price_fixed_rate_bond,
    price_future,
    price_vanilla,
    price_zero_coupon_bond,
)
from ..schemas import (
    AmericanVanillaRequest,
    AsianRequest,
    FixedRateBondRequest,
    FutureRequest,
    PricingResponse,
    VanillaRequest,
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
        },
    )
    return resp
