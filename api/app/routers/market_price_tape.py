"""Price tape — read from the local Postgres filled by `data-ingest` (no cloud,
no live yfinance calls here)."""

from datetime import date, timedelta
from typing import Dict, List

from fastapi import APIRouter, HTTPException, Query
from pydantic import BaseModel
from starlette.concurrency import run_in_threadpool

from .. import db
from ..cache import TTLCache
from ..logging_utils import get_logger
from ..request_context import set_cache_hit
from ..schemas import MarketHistoryPoint, MarketHistoryResponse

router = APIRouter()
logger = get_logger()


class TickersResponse(BaseModel):
    tickers: List[str]


_RANGE_DAYS: Dict[str, int] = {
    "1M": 31,
    "3M": 93,
    "6M": 186,
    "YTD": 0,  # handled specially
    "1Y": 372,
    "2Y": 744,
    "5Y": 1860,
    "max": 100_000,
}

_HISTORY_CACHE = TTLCache[str, MarketHistoryResponse](max_size=512, ttl_seconds=60 * 30)
_TICKERS_CACHE = TTLCache[str, TickersResponse](max_size=1, ttl_seconds=60 * 60)


@router.get("/market/tickers", response_model=TickersResponse)
async def list_tickers() -> TickersResponse:
    cached = _TICKERS_CACHE.get("all")
    if cached:
        set_cache_hit()
        return cached
    try:
        tickers = await run_in_threadpool(db.sp500_tickers)
    except db.StoreUnavailable as exc:
        logger.error("list_tickers: store unavailable", extra={"error": str(exc)})
        raise HTTPException(
            status_code=503, detail="Market data store unavailable"
        ) from exc
    response = TickersResponse(tickers=tickers)
    _TICKERS_CACHE.set("all", response)
    return response


@router.get("/market/prices/history", response_model=MarketHistoryResponse)
async def market_history(
    ticker: str = Query(..., min_length=1, max_length=12),
    range: str = Query("6M", pattern="^(1M|3M|6M|YTD|1Y|2Y|5Y|max)$"),
) -> MarketHistoryResponse:
    ticker = ticker.strip().upper()
    cache_key = f"{ticker}:{range}"

    cached = _HISTORY_CACHE.get(cache_key)
    if cached:
        set_cache_hit()
        return cached

    if range == "YTD":
        since = date(date.today().year, 1, 1)
    else:
        since = date.today() - timedelta(days=_RANGE_DAYS[range])

    try:
        rows = await run_in_threadpool(db.price_history, ticker, since)
    except db.StoreUnavailable as exc:
        logger.error(
            "market_history: store unavailable",
            extra={"ticker": ticker, "error": str(exc)},
        )
        raise HTTPException(
            status_code=503, detail="Market data store unavailable"
        ) from exc

    if not rows:
        raise HTTPException(status_code=404, detail=f"No price history for {ticker}")

    response = MarketHistoryResponse(
        ticker=ticker,
        points=[MarketHistoryPoint(date=d, close=c) for d, c in rows],
    )
    _HISTORY_CACHE.set(cache_key, response)
    logger.info(
        "market_history", extra={"ticker": ticker, "range": range, "points": len(rows)}
    )
    return response
