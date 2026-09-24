"""Router — FX (the FX tab of the market page, and the quanto pricer's inputs).

GET /market/fx/overview?base=EUR&quote=USD     spot, forwards, realised vol
GET /market/fx/history?base=EUR&quote=USD&years=5
GET /market/fx/correlation?ticker=^FCHI&base=EUR&quote=USD&window=3Y

The computation and the methodology text live in `fx.py`.
"""

from datetime import date, timedelta
from typing import Literal

from fastapi import APIRouter, HTTPException, Query

from .. import db, fx
from ..cache import TTLCache
from ..request_context import set_cache_hit
from ..schemas import (
    FxCorrelationResponse,
    FxCurrency,
    FxForwardPoint,
    FxHistoryPoint,
    FxHistoryResponse,
    FxOverviewResponse,
    MethodologySection,
)

router = APIRouter()

_OVERVIEW_CACHE = TTLCache[str, FxOverviewResponse](max_size=50, ttl_seconds=3600)
_HISTORY_CACHE = TTLCache[str, FxHistoryResponse](max_size=50, ttl_seconds=3600)
_CORR_CACHE = TTLCache[str, FxCorrelationResponse](max_size=200, ttl_seconds=3600)


def _unavailable(exc: Exception) -> HTTPException:
    return HTTPException(status_code=404, detail=str(exc))


@router.get("/market/fx/overview", response_model=FxOverviewResponse)
def fx_overview(
    base: FxCurrency = Query("EUR"), quote: FxCurrency = Query("USD")
) -> FxOverviewResponse:
    key = f"{base}{quote}"
    cached = _OVERVIEW_CACHE.get(key)
    if cached is not None:
        set_cache_hit()
        return cached
    try:
        history = fx.pair_history(base, quote, date.today() - timedelta(days=5 * 366))
        spot_date, spot = history.index[-1], float(history.iloc[-1])
        warnings: list[str] = []
        forwards = unavailable = None
        curve_dates: dict = {}
        try:
            fwd = fx.forwards(base, quote, spot, spot_date)
            forwards = [FxForwardPoint(**vars(p)) for p in fwd.points]
            curve_dates = fwd.curve_dates
            for ccy, d in curve_dates.items():
                gap = abs((spot_date - d).days)
                if gap > fx.MAX_DATE_GAP_DAYS:
                    warnings.append(
                        f"The {ccy} curve is from {d.isoformat()}, {gap} days from the "
                        f"spot fixing {spot_date.isoformat()}: the forwards mix two dates."
                    )
        except fx.FxUnavailable as exc:
            unavailable = str(exc)
        vols = {}
        for window, days in fx.WINDOWS.items():
            since = spot_date - timedelta(days=days)
            vols[window] = fx.realised_vol(history[history.index >= since])
    except fx.FxUnavailable as exc:
        raise _unavailable(exc) from exc
    except db.StoreUnavailable as exc:
        raise HTTPException(
            status_code=503, detail="Market data store unavailable"
        ) from exc
    response = FxOverviewResponse(
        base=base,
        quote=quote,
        spot=spot,
        spot_date=spot_date,
        forwards=forwards,
        forwards_unavailable=unavailable,
        curve_dates=curve_dates,
        realised_vol=vols,
        methodology=[
            MethodologySection(title=t, paragraphs=p) for t, p in fx.METHODOLOGY
        ],
        warnings=warnings,
    )
    _OVERVIEW_CACHE.set(key, response)
    return response


@router.get("/market/fx/history", response_model=FxHistoryResponse)
def fx_history(
    base: FxCurrency = Query("EUR"),
    quote: FxCurrency = Query("USD"),
    years: int = Query(5, ge=1, le=30),
) -> FxHistoryResponse:
    key = f"{base}{quote}:{years}"
    cached = _HISTORY_CACHE.get(key)
    if cached is not None:
        set_cache_hit()
        return cached
    try:
        s = fx.pair_history(
            base, quote, date.today() - timedelta(days=int(365.25 * years))
        )
    except fx.FxUnavailable as exc:
        raise _unavailable(exc) from exc
    except db.StoreUnavailable as exc:
        raise HTTPException(
            status_code=503, detail="Market data store unavailable"
        ) from exc
    response = FxHistoryResponse(
        base=base,
        quote=quote,
        points=[FxHistoryPoint(date=d, rate=float(v)) for d, v in s.items()],
    )
    _HISTORY_CACHE.set(key, response)
    return response


@router.get("/market/fx/correlation", response_model=FxCorrelationResponse)
def fx_correlation(
    ticker: str = Query(..., min_length=1, max_length=16),
    base: FxCurrency = Query("EUR"),
    quote: FxCurrency = Query("USD"),
    window: Literal["1Y", "3Y", "5Y"] = Query("3Y"),
    frequency: Literal["weekly", "daily"] = Query("weekly"),
) -> FxCorrelationResponse:
    ticker = ticker.strip().upper()
    key = f"{ticker}:{base}{quote}:{window}:{frequency}"
    cached = _CORR_CACHE.get(key)
    if cached is not None:
        set_cache_hit()
        return cached
    since = fx.window_start(window)
    try:
        c = fx.correlation(
            fx.asset_series(ticker, since),
            fx.pair_history(base, quote, since),
            frequency,
        )
    except fx.FxUnavailable as exc:
        raise _unavailable(exc) from exc
    except db.StoreUnavailable as exc:
        raise HTTPException(
            status_code=503, detail="Market data store unavailable"
        ) from exc
    response = FxCorrelationResponse(
        ticker=ticker,
        base=base,
        quote=quote,
        window=window,
        frequency=frequency,
        **vars(c),
    )
    _CORR_CACHE.set(key, response)
    return response
