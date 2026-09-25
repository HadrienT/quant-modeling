import asyncio
from typing import Callable, TypeVar

from fastapi import APIRouter, HTTPException
from pydantic import BaseModel
from starlette.concurrency import run_in_threadpool

from .. import telemetry, valuation
from ..audit import emit
from ..pricing_service import validate_script
from ..request_context import current_ip_hash
from ..schemas import (
    AmericanVanillaRequest,
    AsianRequest,
    AutocallRequest,
    BarrierRequest,
    BasketRequest,
    CommodityForwardRequest,
    CommodityOptionRequest,
    DatedAsianRequest,
    DigitalRequest,
    DispersionSwapRequest,
    FXForwardRequest,
    FXOptionRequest,
    FixedRateBondRequest,
    FutureRequest,
    LookbackRequest,
    MountainRequest,
    PricingResponse,
    QuantoRequest,
    RainbowRequest,
    ScriptRequest,
    ScriptValidateRequest,
    ScriptValidateResponse,
    ScriptedProductRequest,
    VanillaRequest,
    VarianceSwapRequest,
    VolatilitySwapRequest,
    ZeroCouponBondRequest,
)

router = APIRouter()
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


async def _price(
    product_id: str,
    req: BaseModel,
    user_errors: tuple[type[Exception], ...] = (),
) -> PricingResponse:
    """Every pricing endpoint goes through here (blueprint WP 18e): the
    pricing runs under the market-inputs collector and the `engine.price`
    span, and a successful one is recorded as a `pricing.valuation` event —
    what a replay re-runs (valuation.py). Its duration and failures feed
    `qm_pricing_duration_seconds` and `qm_pricing_errors_total`.

    `user_errors` are exceptions the pricer raises for a bad input (a
    malformed script, missing market data): they become a 422."""
    product = valuation.PRODUCTS[product_id]
    labels = {
        "product": product_id,
        "engine": product.engine(req),
        "model": product.model(req),
    }
    try:
        priced = await _run_with_timeout(valuation.price, product_id, req)
    except HTTPException as exc:
        telemetry.pricing_errors.add(
            1,
            {
                "product": product_id,
                "code": "timeout" if exc.status_code == 408 else str(exc.status_code),
            },
        )
        raise
    except user_errors as exc:
        telemetry.pricing_errors.add(
            1, {"product": product_id, "code": "invalid_input"}
        )
        raise HTTPException(status_code=422, detail=str(exc)) from exc
    except Exception:
        telemetry.pricing_errors.add(1, {"product": product_id, "code": "internal"})
        raise
    telemetry.pricing_duration.record(priced.duration_s, labels)
    emit(
        "pricing.valuation",
        valuation.payload(product_id, req, priced, ip_hash=current_ip_hash()),
    )
    return priced.response.model_copy(update={"compute_ms": priced.duration_s * 1000})


@router.post("/price/option/vanilla", response_model=PricingResponse)
async def price_vanilla_endpoint(req: VanillaRequest) -> PricingResponse:
    return await _price("vanilla", req)


@router.post("/price/option/american-vanilla", response_model=PricingResponse)
async def price_american_vanilla_endpoint(
    req: AmericanVanillaRequest,
) -> PricingResponse:
    return await _price("american_vanilla", req)


@router.post("/price/option/asian", response_model=PricingResponse)
async def price_asian_endpoint(req: AsianRequest) -> PricingResponse:
    return await _price("asian", req)


@router.post("/price/option/dated-asian", response_model=PricingResponse)
async def price_dated_asian_endpoint(req: DatedAsianRequest) -> PricingResponse:
    return await _price("dated_asian", req)


@router.post("/price/scripted", response_model=PricingResponse)
async def price_script_endpoint(req: ScriptRequest) -> PricingResponse:
    """blueprint/wp/16-scripting.md §8.3. Not under /price/option/* — kept as
    its own top-level path, mirroring the language's own scope. A malformed
    script (ScriptError) or a bad market input (InvalidInput, missing market
    data) is a user-input error, not a server failure."""
    return await _price("script", req, user_errors=(RuntimeError, ValueError))


@router.post("/price/scripted-product", response_model=PricingResponse)
async def price_scripted_product_endpoint(
    req: ScriptedProductRequest,
) -> PricingResponse:
    """A product of the script library from its term sheet
    (blueprint/wp/16-scripting.md §8.8); the response carries the script
    priced. Pricing the same terms under several `model`s with one `seed`
    compares the models on common random numbers."""
    return await _price("scripted_product", req, user_errors=(RuntimeError, ValueError))


@router.post("/price/scripted/validate", response_model=ScriptValidateResponse)
async def validate_script_endpoint(
    req: ScriptValidateRequest,
) -> ScriptValidateResponse:
    """Parse-only companion to /price/scripted: no market inputs, no
    simulation — an editor's "Validate" action against this is instant."""
    try:
        return await _run_with_timeout(validate_script, req)
    except (RuntimeError, ValueError) as exc:
        raise HTTPException(status_code=422, detail=str(exc)) from exc


@router.post("/price/option/barrier", response_model=PricingResponse)
async def price_barrier_endpoint(req: BarrierRequest) -> PricingResponse:
    return await _price("barrier", req)


@router.post("/price/option/digital", response_model=PricingResponse)
async def price_digital_endpoint(req: DigitalRequest) -> PricingResponse:
    return await _price("digital", req)


@router.post("/price/option/lookback", response_model=PricingResponse)
async def price_lookback_endpoint(req: LookbackRequest) -> PricingResponse:
    return await _price("lookback", req)


@router.post("/price/option/basket", response_model=PricingResponse)
async def price_basket_endpoint(req: BasketRequest) -> PricingResponse:
    return await _price("basket", req)


@router.post("/price/future", response_model=PricingResponse)
async def price_future_endpoint(req: FutureRequest) -> PricingResponse:
    return await _price("future", req)


@router.post("/price/bond/zero-coupon", response_model=PricingResponse)
async def price_zero_coupon_bond_endpoint(
    req: ZeroCouponBondRequest,
) -> PricingResponse:
    return await _price("zero_coupon_bond", req)


@router.post("/price/bond/fixed-rate", response_model=PricingResponse)
async def price_fixed_rate_bond_endpoint(req: FixedRateBondRequest) -> PricingResponse:
    return await _price("fixed_rate_bond", req)


@router.post("/price/structured/autocall", response_model=PricingResponse)
async def price_autocall_endpoint(req: AutocallRequest) -> PricingResponse:
    return await _price("autocall", req)


@router.post("/price/structured/mountain", response_model=PricingResponse)
async def price_mountain_endpoint(req: MountainRequest) -> PricingResponse:
    return await _price("mountain", req)


@router.post("/price/volatility/variance-swap", response_model=PricingResponse)
async def price_variance_swap_endpoint(req: VarianceSwapRequest) -> PricingResponse:
    return await _price("variance_swap", req)


@router.post("/price/volatility/volatility-swap", response_model=PricingResponse)
async def price_volatility_swap_endpoint(req: VolatilitySwapRequest) -> PricingResponse:
    return await _price("volatility_swap", req)


@router.post("/price/volatility/dispersion-swap", response_model=PricingResponse)
async def price_dispersion_swap_endpoint(req: DispersionSwapRequest) -> PricingResponse:
    return await _price("dispersion_swap", req)


@router.post("/price/fx/forward", response_model=PricingResponse)
async def price_fx_forward_endpoint(req: FXForwardRequest) -> PricingResponse:
    return await _price("fx_forward", req)


@router.post("/price/option/quanto", response_model=PricingResponse)
async def price_quanto_endpoint(req: QuantoRequest) -> PricingResponse:
    """A foreign asset paid in the domestic currency at a fixed rate: Black-Scholes
    with the quanto drift adjustment. `rho` in the greeks is the domestic-rate rho;
    the model's inputs (rates, vols, correlation) are the caller's."""
    return await _price("quanto", req, user_errors=(RuntimeError, ValueError))


@router.post("/price/fx/option", response_model=PricingResponse)
async def price_fx_option_endpoint(req: FXOptionRequest) -> PricingResponse:
    return await _price("fx_option", req)


@router.post("/price/commodity/forward", response_model=PricingResponse)
async def price_commodity_forward_endpoint(
    req: CommodityForwardRequest,
) -> PricingResponse:
    return await _price("commodity_forward", req)


@router.post("/price/commodity/option", response_model=PricingResponse)
async def price_commodity_option_endpoint(
    req: CommodityOptionRequest,
) -> PricingResponse:
    return await _price("commodity_option", req)


@router.post("/price/option/rainbow", response_model=PricingResponse)
async def price_rainbow_endpoint(req: RainbowRequest) -> PricingResponse:
    return await _price("rainbow", req)
