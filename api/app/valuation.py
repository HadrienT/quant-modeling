"""The valuation record — blueprint WP 18e (`blueprint/wp/18-observability.md` §6).

A valuation is a function

    price = f(instrument, market data, model configuration, code version, seed)

and every pricing emits a `pricing.valuation` event that keeps all five, so it
can be replayed (`replay.py`). This module owns:

- `PRODUCTS`: for each pricing endpoint, its request model, its pricing
  function, and the model and engine that function *actually* runs for a given
  request (mirroring `pricing_service.py`'s own dispatch, not the request's
  wishes: an American vanilla asked for with `engine="mc"` runs on a binomial
  tree, and the record says so). The router and the replay endpoint both go
  through it, so what is recorded is exactly what is re-run.
- the market inputs collector: code that reads the market database during a
  pricing (`market_snapshot.py`) calls `record_market_input()`, and the inputs
  land in the record with their date, source, status and a hash of the value.
- `price()`: runs one pricing under the collector, inside the `engine.price`
  span, and returns the response with what the record needs.
"""

from __future__ import annotations

import hashlib
import json
import os
import time
from contextvars import ContextVar
from dataclasses import dataclass, field
from typing import Any, Callable

from pydantic import BaseModel

from . import pricing_service as ps
from . import schemas as s
from .audit.envelope import lib_build_sha
from .audit.payloads import (
    CodeVersion,
    EngineSpec,
    MarketInput,
    MarketInputStatus,
    ModelSpec,
    ValuationPayload,
    ValuationResult,
    ValuationTiming,
)
from .schemas import PricingResponse
from .telemetry import tracer

# ── Hashes ───────────────────────────────────────────────────────────────────


def _canonical(value: Any) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), default=str).encode(
        "utf-8"
    )


def value_hash(value: Any) -> str:
    """sha256 of a canonical JSON form: equal values hash equal whatever
    their key order, so a replay compares data, not formatting."""
    return "sha256:" + hashlib.sha256(_canonical(value)).hexdigest()


# ── Market inputs collector ──────────────────────────────────────────────────

_market_inputs: ContextVar[list[MarketInput] | None] = ContextVar(
    "market_inputs", default=None
)


def record_market_input(
    name: str,
    source: str,
    as_of: str | None,
    status: MarketInputStatus,
    value: Any,
) -> None:
    """Called where a pricing reads market data. Outside a pricing (e.g. a
    surface endpoint) there is no collector and this does nothing."""
    inputs = _market_inputs.get()
    if inputs is not None:
        inputs.append(
            MarketInput(
                name=name,
                source=source,
                as_of=as_of,
                status=status,
                value_hash=value_hash(value),
            )
        )


# ── Products ─────────────────────────────────────────────────────────────────


def _const(value: str) -> Callable[[Any], str]:
    return lambda _req: value


def _mc_or_analytic(req: Any) -> str:
    return "mc" if req.engine == s.EngineType.mc else "analytic"


def _tree(req: Any) -> str:
    return "trinomial" if req.engine.value == "trinomial" else "binomial"


def _vanilla_engine(req: s.VanillaRequest) -> str:
    # price_vanilla: American → trees only; European → the requested engine.
    return _tree(req) if req.is_american else req.engine.value


@dataclass(frozen=True)
class Product:
    request: type[BaseModel]
    price: Callable[[Any], PricingResponse]
    model: Callable[[Any], str]
    engine: Callable[[Any], str]
    scheme: Callable[[Any], str | None] = lambda _req: None
    #: The pricing function opens its own `market_snapshot.load` and
    #: `engine.price` spans (it reads market data before pricing).
    own_spans: bool = False


PRODUCTS: dict[str, Product] = {
    "vanilla": Product(
        s.VanillaRequest, ps.price_vanilla, _const("black_scholes"), _vanilla_engine
    ),
    "american_vanilla": Product(
        s.AmericanVanillaRequest,
        ps.price_american_vanilla,
        _const("black_scholes"),
        _tree,
    ),
    "asian": Product(
        s.AsianRequest, ps.price_asian, _const("black_scholes"), _mc_or_analytic
    ),
    "dated_asian": Product(
        s.DatedAsianRequest,
        ps.price_dated_asian,
        _const("black_scholes"),
        _const("mc"),
        scheme=lambda r: r.sampler,
    ),
    "script": Product(
        s.ScriptRequest,
        ps.price_script,
        lambda r: r.model,
        _const("mc"),
        scheme=lambda r: r.sampler,
        own_spans=True,
    ),
    "scripted_product": Product(
        s.ScriptedProductRequest,
        ps.price_scripted_product,
        lambda r: r.model,
        _const("mc"),
        scheme=lambda r: r.sampler,
        own_spans=True,
    ),
    "barrier": Product(
        s.BarrierRequest, ps.price_barrier, _const("black_scholes"), _const("mc")
    ),
    "digital": Product(
        s.DigitalRequest, ps.price_digital, _const("black_scholes"), _const("analytic")
    ),
    "lookback": Product(
        s.LookbackRequest, ps.price_lookback, _const("black_scholes"), _const("mc")
    ),
    "basket": Product(
        s.BasketRequest, ps.price_basket, _const("black_scholes"), _const("mc")
    ),
    "future": Product(
        s.FutureRequest, ps.price_future, _const("black_scholes"), _const("analytic")
    ),
    "zero_coupon_bond": Product(
        s.ZeroCouponBondRequest,
        ps.price_zero_coupon_bond,
        _const("flat_rate"),
        _const("analytic"),
    ),
    "fixed_rate_bond": Product(
        s.FixedRateBondRequest,
        ps.price_fixed_rate_bond,
        _const("flat_rate"),
        _const("analytic"),
    ),
    "autocall": Product(
        s.AutocallRequest, ps.price_autocall, _const("black_scholes"), _const("mc")
    ),
    "mountain": Product(
        s.MountainRequest, ps.price_mountain, _const("black_scholes"), _const("mc")
    ),
    "variance_swap": Product(
        s.VarianceSwapRequest,
        ps.price_variance_swap,
        _const("black_scholes"),
        _mc_or_analytic,
    ),
    "volatility_swap": Product(
        s.VolatilitySwapRequest,
        ps.price_volatility_swap,
        _const("black_scholes"),
        _const("mc"),
    ),
    "dispersion_swap": Product(
        s.DispersionSwapRequest,
        ps.price_dispersion_swap,
        _const("black_scholes"),
        _const("mc"),
    ),
    "fx_forward": Product(
        s.FXForwardRequest,
        ps.price_fx_forward,
        _const("garman_kohlhagen"),
        _const("analytic"),
    ),
    "fx_option": Product(
        s.FXOptionRequest,
        ps.price_fx_option,
        _const("garman_kohlhagen"),
        _const("analytic"),
    ),
    "commodity_forward": Product(
        s.CommodityForwardRequest,
        ps.price_commodity_forward,
        _const("cost_of_carry"),
        _const("analytic"),
    ),
    "commodity_option": Product(
        s.CommodityOptionRequest,
        ps.price_commodity_option,
        _const("black76"),
        _const("analytic"),
    ),
    "quanto": Product(
        s.QuantoRequest,
        ps.price_quanto,
        _const("quanto_black_scholes"),
        lambda r: r.engine,
    ),
    "rainbow": Product(
        s.RainbowRequest, ps.price_rainbow, _const("black_scholes"), _const("mc")
    ),
}


def engine_spec(product: Product, req: Any, device: str | None = None) -> EngineSpec:
    name = product.engine(req)
    mc = name == "mc"
    return EngineSpec(
        name=name,
        n_paths=getattr(req, "n_paths", None) if mc else None,
        seed=getattr(req, "seed", None) if mc else None,
        scheme=product.scheme(req),
        device=device if mc else None,
    )


# ── One pricing ──────────────────────────────────────────────────────────────


@dataclass
class Priced:
    response: PricingResponse
    market_inputs: list[MarketInput] = field(default_factory=list)
    duration_s: float = 0.0


def price(product_id: str, req: BaseModel) -> Priced:
    """Runs one pricing, collecting the market inputs it reads. Synchronous:
    the router runs it in the thread pool, the replay endpoint too."""
    product = PRODUCTS[product_id]
    inputs: list[MarketInput] = []
    token = _market_inputs.set(inputs)
    start = time.perf_counter()
    try:
        if product.own_spans:
            response = product.price(req)
        else:
            with tracer.start_as_current_span(
                "engine.price",
                attributes={
                    "qm.product": product_id,
                    "qm.model": product.model(req),
                    "qm.engine": product.engine(req),
                },
            ):
                response = product.price(req)
    finally:
        _market_inputs.reset(token)
    return Priced(response, inputs, time.perf_counter() - start)


def _model_spec(product: Product, req: BaseModel, resp: PricingResponse) -> ModelSpec:
    """The model the pricing actually ran: for a script priced with
    model='auto', the one chosen, with what was calibrated for it (a replay
    re-derives both from the same stored snapshot)."""
    choice = resp.model_choice
    if choice is None:
        return ModelSpec(name=product.model(req))
    params: dict[str, str | int | float | None] = {"requested": choice.requested}
    calibration_id = None
    for i, u in enumerate(choice.underlyings or []):
        params[f"vol[{i}]"] = u.vol
        if u.ticker:
            params[f"ticker[{i}]"] = u.ticker
    cal = choice.calibration
    if cal is not None:
        calibration_id = f"{cal.ticker}:{cal.snapshot.isoformat()}"
        if cal.heston is not None:
            params.update(
                {
                    k: getattr(cal.heston, k)
                    for k in ("v0", "kappa", "theta", "xi", "rho")
                }
            )
            params["heston_iv_rmse"] = cal.heston.iv_rmse
    return ModelSpec(name=choice.model, params=params, calibration_id=calibration_id)


def payload(
    product_id: str, req: BaseModel, priced: Priced, *, ip_hash: str | None
) -> ValuationPayload:
    product = PRODUCTS[product_id]
    request = req.model_dump(mode="json")
    resp = priced.response
    return ValuationPayload(
        product=product_id,
        request=request,
        request_hash=value_hash(request),
        model=_model_spec(product, req, resp),
        engine=engine_spec(product, req, resp.device),
        market_inputs=priced.market_inputs,
        result=ValuationResult(
            npv=resp.npv,
            mc_std_error=resp.mc_std_error,
            greeks=resp.greeks.model_dump(mode="json"),
        ),
        timing=ValuationTiming(duration_ms=priced.duration_s * 1000),
        code=CodeVersion(
            api_sha=os.getenv("COMMIT_SHA", "dev"), lib_build=lib_build_sha()
        ),
        ip_hash=ip_hash,
    )
