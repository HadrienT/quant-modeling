import asyncio
import functools
import os
import platform
import threading
import time
from typing import Callable, TypeVar

import quantmodeling as qm

from fastapi import APIRouter, HTTPException
from pydantic import BaseModel
from starlette.concurrency import run_in_threadpool

from .. import cancellation, convergence, greeks_fallback, telemetry, valuation
from ..audit import emit
from ..logging_utils import get_logger
from ..pricing_service import validate_script
from ..rates_derivatives_schemas import (
    SwapPricingRequest,
    SwapPricingResponse,
    SwaptionPricingRequest,
    SwaptionPricingResponse,
)
from ..request_context import current_ip_hash
from ..schemas import (
    ComputeDevicesResponse,
    ComputeTimings,
    ConvergenceResponse,
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
    MarketVegaResponse,
    RiskProfileRequest,
    RiskProfileResponse,
    RiskProfileRow,
    VanillaRequest,
    VarianceSwapRequest,
    VolatilitySwapRequest,
    ZeroCouponBondRequest,
)

router = APIRouter()
#: A pricing's computing time, from when it has a slot, before a 408.
_PRICING_TIMEOUT_SECONDS = 20
#: Pricings computing at once (issue #347). Without a bound every request ran
#: at once on the container's 2 CPUs and 2 GB, each slower and all their
#: memory added up. Three: two CPU-heavy pricings side by side, and a third,
#: usually on the GPU, which needs little CPU.
_PRICING_SLOTS = int(os.environ.get("QM_PRICING_SLOTS") or 3)
#: How long a pricing waits for a slot before a 429 (the server is busy).
_SLOT_WAIT_SECONDS = float(os.environ.get("QM_PRICING_SLOT_WAIT_S") or 10)
#: Held by the worker thread for as long as it computes -- past a 408 too, so
#: an abandoned pricing still counts until it has stopped.
_slots = threading.BoundedSemaphore(_PRICING_SLOTS)
_T = TypeVar("_T")


logger = get_logger()


class _Busy(Exception):
    """Every pricing slot stayed taken for _SLOT_WAIT_SECONDS."""


async def _run_with_timeout(func: Callable[..., _T], *args, slot: bool = True) -> _T:
    """`func(*args)` in a worker thread, once it has one of the pricing slots
    (`slot=False`: a parse, no slot), within _PRICING_TIMEOUT_SECONDS of
    computing. No slot within _SLOT_WAIT_SECONDS: 429. Past the timeout: 408,
    and the pricing's cancellation flag is set, so it stops at its next
    check (cancellation.py) instead of running on with nobody waiting; it
    keeps its slot until it has stopped."""
    flag = threading.Event()
    loop = asyncio.get_running_loop()

    def _start_the_clock() -> None:
        try:
            timeout.reschedule(loop.time() + _PRICING_TIMEOUT_SECONDS)
        except RuntimeError:  # the job already finished and the block exited
            pass

    def job() -> _T:
        if slot and not _slots.acquire(timeout=_SLOT_WAIT_SECONDS):
            raise _Busy()
        try:
            if flag.is_set():  # abandoned while it waited
                raise cancellation.Cancelled("abandoned before it started")
            # the computing time starts now, not when the request came
            loop.call_soon_threadsafe(_start_the_clock)
            return cancellation.run_with(
                flag,
                func,
                *args,
                deadline=time.perf_counter() + _PRICING_TIMEOUT_SECONDS,
            )
        finally:
            if slot:
                _slots.release()

    try:
        # first the wait for a slot (the job gives up on its own at
        # _SLOT_WAIT_SECONDS), then the job moves the deadline
        async with asyncio.timeout(_SLOT_WAIT_SECONDS + 5.0) as timeout:
            return await run_in_threadpool(job)
    except _Busy as exc:
        logger.warning(
            "pricing refused: every slot busy", extra={"slots": _PRICING_SLOTS}
        )
        raise HTTPException(
            status_code=429,
            detail=f"The pricing server is busy: every slot ({_PRICING_SLOTS}) is taken. "
            "Retry in a few seconds.",
        ) from exc
    except TimeoutError as exc:
        flag.set()
        raise HTTPException(
            status_code=408,
            detail=f"Pricing timed out after {_PRICING_TIMEOUT_SECONDS} seconds. Reduce paths or grid steps and retry.",
        ) from exc
    except BaseException:
        flag.set()  # the request went away: nobody reads the result
        raise


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
    telemetry.pricing_duration.record(
        priced.duration_s, {**labels, "device": priced.response.device}
    )
    emit(
        "pricing.valuation",
        valuation.payload(product_id, req, priced, ip_hash=current_ip_hash()),
    )
    response = priced.response.model_copy(
        update={
            "compute_ms": priced.duration_s * 1000,
            "timings": _timings(priced),
        }
    )
    if product_id not in _OWN_GREEKS:
        start = time.perf_counter()
        response = await _with_every_greek(product, req, response)
        response.timings.greeks_ms = (time.perf_counter() - start) * 1000
    return response


def _timings(priced: valuation.Priced) -> ComputeTimings:
    """The pricing's phases in milliseconds, the remainder as `other`."""
    ms = {k: v * 1000 for k, v in priced.phases.items()}
    market, calibration, engine = (
        ms.get("market", 0.0),
        ms.get("calibration", 0.0),
        ms.get("engine", 0.0),
    )
    return ComputeTimings(
        market_ms=market,
        calibration_ms=calibration,
        engine_ms=engine,
        other_ms=max(priced.duration_s * 1000 - market - calibration - engine, 0.0),
    )


#: Products whose sensitivities are not the five greeks of an option, or come
#: by another route: rates (PV01, the workbench's own panel) and scripts (the
#: adjoint risk report, or their own bumps).
_OWN_GREEKS = frozenset(
    {"interest_rate_swap", "swaption", "script", "scripted_product", "dated_asian"}
)


async def _with_every_greek(
    product: valuation.Product, req: BaseModel, response: PricingResponse
) -> PricingResponse:
    """The greeks the engine left out, by bumping and revaluing
    (greeks_fallback.py). They are an addition to the answer: when the bumps
    fail or run out of time, the pricing is returned as it is."""

    def run() -> PricingResponse:
        completed, names = greeks_fallback.complete(
            req, response, lambda r: product.price(r).npv
        )
        return completed.model_copy(update={"bumped_greeks": names})

    try:
        return await _run_with_timeout(run)
    except Exception:  # noqa: BLE001 — never lose a price to its greeks
        logger.warning("greeks by bump failed", exc_info=True)
        return response


@router.post("/price/option/vanilla", response_model=PricingResponse)
async def price_vanilla_endpoint(req: VanillaRequest) -> PricingResponse:
    # device="gpu" on a server without one is the caller's to fix: a 422 that
    # says so, not a 500 (blueprint/wp/19-gpu.md §8).
    return await _price("vanilla", req, user_errors=(qm.GpuUnavailable,))


@functools.cache
def _cpu_model() -> str:
    try:
        with open("/proc/cpuinfo", encoding="utf-8") as f:
            for line in f:
                if line.startswith("model name"):
                    return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor() or "CPU"


@router.get("/price/devices", response_model=ComputeDevicesResponse)
def compute_devices_endpoint() -> ComputeDevicesResponse:
    """What the pricing engines can run on, for the device selector."""
    return ComputeDevicesResponse(
        cpu=_cpu_model(),
        gpus=list(qm.gpu_devices()),
        gpu_compiled=bool(qm.gpu_compiled()),
    )


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


@router.post("/price/risk-profile", response_model=RiskProfileResponse)
async def risk_profile_endpoint(req: RiskProfileRequest) -> RiskProfileResponse:
    """Long or short what, for a library product: every market parameter
    bumped on a reference market, the sign of the price change (holder's
    side), with paired Monte-Carlo errors (risk_profile.py). Cached per
    product and terms. Under /price/ like every computation: the production
    nginx (and the dev proxy) only forward /api/, /market/ and /price/."""
    from .. import risk_profile

    try:
        price, se, rows = await _run_with_timeout(
            risk_profile.risk_profile, req.product, req.terms
        )
    except (RuntimeError, ValueError) as exc:
        raise HTTPException(status_code=422, detail=str(exc)) from exc
    return RiskProfileResponse(
        product=req.product,
        price=price,
        price_std_error=se,
        reference=risk_profile.REFERENCE,
        rows=[RiskProfileRow(**r.__dict__) for r in rows],
        method=(
            f"Bump and reprice on {risk_profile.BATCHES} batches of "
            f"{risk_profile.PATHS_PER_BATCH:,} paths with common random numbers; "
            "levels fixed at the reference spot on the trade date."
        ),
    )


@router.post("/price/market-vega", response_model=MarketVegaResponse)
async def market_vega_endpoint(req: ScriptedProductRequest) -> MarketVegaResponse:
    """Vega by quoted option (market_vega.py, blueprint/wp/17-aad.md §11):
    the product under the local vol of the ticker's stored chain,
    differentiated by AAD, then through Dupire and each SVI fit to every
    quoted implied vol. One underlying, a ticker."""
    from .. import market_vega

    try:
        return await _run_with_timeout(market_vega.market_vega, req)
    except (RuntimeError, ValueError) as exc:
        raise HTTPException(status_code=422, detail=str(exc)) from exc


@router.post("/price/scripted/validate", response_model=ScriptValidateResponse)
async def validate_script_endpoint(
    req: ScriptValidateRequest,
) -> ScriptValidateResponse:
    """Parse-only companion to /price/scripted: no market inputs, no
    simulation — an editor's "Validate" action against this is instant."""
    try:
        return await _run_with_timeout(validate_script, req, slot=False)
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


# Rates: priced on the quotes the request carries. A RuntimeError is the C++
# refusing them (quotes no curve can reprice, a swap past the curve...): the
# user's input, not a server fault.
@router.post("/price/rates/swap", response_model=SwapPricingResponse)
async def price_interest_rate_swap_endpoint(
    req: SwapPricingRequest,
) -> PricingResponse:
    return await _price("interest_rate_swap", req, user_errors=(RuntimeError,))


@router.post("/price/rates/swaption", response_model=SwaptionPricingResponse)
async def price_swaption_endpoint(req: SwaptionPricingRequest) -> PricingResponse:
    return await _price("swaption", req, user_errors=(RuntimeError,))


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


#: The Monte-Carlo pricing routes, each with a sibling `<route>/convergence`
#: taking the same request (convergence.py). A test checks that this covers
#: every product with a path count.
MC_ROUTES = {
    "/price/option/vanilla": "vanilla",
    "/price/option/american-vanilla": "american_vanilla",
    "/price/option/asian": "asian",
    "/price/option/dated-asian": "dated_asian",
    "/price/scripted": "script",
    "/price/scripted-product": "scripted_product",
    "/price/option/barrier": "barrier",
    "/price/option/lookback": "lookback",
    "/price/option/basket": "basket",
    "/price/structured/autocall": "autocall",
    "/price/structured/mountain": "mountain",
    "/price/volatility/variance-swap": "variance_swap",
    "/price/volatility/volatility-swap": "volatility_swap",
    "/price/volatility/dispersion-swap": "dispersion_swap",
    "/price/option/quanto": "quanto",
    "/price/option/rainbow": "rainbow",
}


def _convergence_route(product_id: str):
    request_model = valuation.PRODUCTS[product_id].request

    async def endpoint(req: request_model) -> ConvergenceResponse:  # type: ignore[valid-type]
        """The engine's standard error measured at N/32 ... N/2 paths, each
        point its own pricing: about the cost of the pricing itself. The
        point at N is the pricing route's own answer."""
        try:
            return await _run_with_timeout(convergence.study, product_id, req)
        except (convergence.NotMonteCarlo, qm.GpuUnavailable) as exc:
            raise HTTPException(status_code=422, detail=str(exc)) from exc
        except (RuntimeError, ValueError) as exc:
            if product_id in ("script", "scripted_product"):
                raise HTTPException(status_code=422, detail=str(exc)) from exc
            raise

    endpoint.__name__ = f"convergence_{product_id}"
    return endpoint


for _path, _product_id in MC_ROUTES.items():
    router.add_api_route(
        f"{_path}/convergence",
        _convergence_route(_product_id),
        methods=["POST"],
        response_model=ConvergenceResponse,
    )
