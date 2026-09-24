"""Router — interest rates by currency (the Rates tab of the market page).

GET /market/rates/overview?currency=EUR   government curve + reference rates
                                          + the methodology the page shows
GET /market/rates/history?currency=EUR&series_id=EUR.ESTR&years=5

The computation and the methodology text live in `rates.py`; this module only
caches and shapes the responses. Rates are decimals.
"""

from typing import Literal

from fastapi import APIRouter, HTTPException, Query

from .. import db, rates
from ..cache import TTLCache
from ..request_context import set_cache_hit
from ..schemas import (
    BenchmarkRate,
    GovernmentCurveResponse,
    MethodologySection,
    QuotedRatePoint,
    RateHistoryPoint,
    RatePoint,
    RatesHistoryResponse,
    RatesOverviewResponse,
)

router = APIRouter()

# Sources publish at most daily: an hour of cache costs nothing in freshness.
_OVERVIEW_CACHE = TTLCache[str, RatesOverviewResponse](max_size=40, ttl_seconds=3600)
_HISTORY_CACHE = TTLCache[str, RatesHistoryResponse](max_size=60, ttl_seconds=3600)

CurrencyParam = Literal["USD", "EUR", "GBP", "CHF", "JPY"]


def _government(
    curve: rates.GovernmentCurve, result: rates.CurveResult, forward_period: float
) -> GovernmentCurveResponse:
    return GovernmentCurveResponse(
        name=curve.name,
        source=curve.source,
        source_url=curve.source_url,
        quote=curve.quote,
        as_of=result.as_of,
        quoted=[QuotedRatePoint(**vars(q)) for q in result.quoted],
        zero=(
            None if result.zero is None else [RatePoint(**vars(p)) for p in result.zero]
        ),
        forward=(
            None
            if result.forward is None
            else [RatePoint(**vars(p)) for p in result.forward]
        ),
        forward_period_years=forward_period,
        no_derivation=curve.no_derivation,
    )


@router.get("/market/rates/overview", response_model=RatesOverviewResponse)
def rates_overview(
    currency: CurrencyParam = Query("USD"),
    forward_period_years: float = Query(
        0.5, gt=0.0, le=10.0, description="Length Δ of the forward rates, in years."
    ),
) -> RatesOverviewResponse:
    forward_period = round(forward_period_years, 6)
    key = f"{currency}:{forward_period}"
    cached = _OVERVIEW_CACHE.get(key)
    if cached is not None:
        set_cache_hit()
        return cached

    spec = rates.CATALOG[currency]
    warnings: list[str] = []
    government = None
    result = None
    unavailable = spec.no_government
    try:
        if spec.government is not None:
            try:
                result = rates.government_curve(spec.government, forward_period)
                government = _government(spec.government, result, forward_period)
                stale = rates.staleness_warning(result.as_of)
                if stale:
                    warnings.append(stale)
            except rates.RatesUnavailable as exc:
                unavailable = str(exc)
                warnings.append(str(exc))
        values = rates.benchmarks(spec)
    except db.StoreUnavailable as exc:
        raise HTTPException(status_code=503, detail="Rates store unavailable") from exc
    except RuntimeError as exc:  # the C++ bootstrap refusing a quote set
        raise HTTPException(
            status_code=422, detail=f"{currency} curve could not be built: {exc}"
        ) from exc

    missing = [b.series_id for b in values if b.rate is None]
    if missing:
        warnings.append(f"Not in the store yet: {', '.join(missing)}.")

    response = RatesOverviewResponse(
        currency=currency,
        government=government,
        government_unavailable=unavailable,
        benchmarks=[BenchmarkRate(**vars(b)) for b in values],
        headline_series=spec.headline,
        methodology=[
            MethodologySection(title=t, paragraphs=p)
            for t, p in rates.methodology(currency, result)
        ],
        warnings=warnings,
    )
    _OVERVIEW_CACHE.set(key, response)
    return response


@router.get("/market/rates/history", response_model=RatesHistoryResponse)
def rates_history(
    currency: CurrencyParam = Query(...),
    series_id: str = Query(..., description="One of the currency's reference rates."),
    years: int = Query(5, ge=1, le=30),
) -> RatesHistoryResponse:
    key = f"{currency}:{series_id}:{years}"
    cached = _HISTORY_CACHE.get(key)
    if cached is not None:
        set_cache_hit()
        return cached
    try:
        points = rates.history(currency, series_id, years)
    except KeyError as exc:
        raise HTTPException(
            status_code=404, detail=f"{series_id} is not a {currency} reference rate"
        ) from exc
    except db.StoreUnavailable as exc:
        raise HTTPException(status_code=503, detail="Rates store unavailable") from exc
    response = RatesHistoryResponse(
        currency=currency,
        series_id=series_id,
        points=[RateHistoryPoint(date=d, rate=r) for d, r in points],
    )
    _HISTORY_CACHE.set(key, response)
    return response
