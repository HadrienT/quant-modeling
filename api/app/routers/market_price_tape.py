"""Price tape — sourced from yfinance (no cloud, blueprint self-hosting)."""

from typing import Dict, List

import pandas as pd
import yfinance as yf
from fastapi import APIRouter, HTTPException, Query
from pydantic import BaseModel
from starlette.concurrency import run_in_threadpool

from ..cache import TTLCache
from ..logging_utils import get_logger
from ..request_context import set_cache_hit
from ..schemas import MarketHistoryPoint, MarketHistoryResponse

router = APIRouter()
logger = get_logger()


class TickersResponse(BaseModel):
    tickers: List[str]


# Curated liquid universe. yfinance has no "list every ticker" endpoint, and a
# self-hosted deployment does not need one — these cover the demo surface.
_UNIVERSE: List[str] = [
    "AAPL", "MSFT", "NVDA", "AMZN", "GOOGL", "META", "TSLA", "AVGO", "JPM", "V",
    "MA", "UNH", "XOM", "CVX", "LLY", "HD", "COST", "PG", "KO", "PEP",
    "NFLX", "AMD", "INTC", "CSCO", "CRM", "ORCL", "ADBE", "QCOM", "TXN", "IBM",
    "BAC", "WFC", "GS", "MS", "DIS", "NKE", "MCD", "SBUX", "BA", "CAT",
    "SPY", "QQQ", "IWM", "DIA", "GLD", "TLT", "HYG", "VTI", "EFA", "EEM",
]

_RANGE_PERIOD: Dict[str, str] = {
    "1M": "1mo",
    "3M": "3mo",
    "6M": "6mo",
    "YTD": "ytd",
    "1Y": "1y",
    "2Y": "2y",
    "5Y": "5y",
    "max": "max",
}

_HISTORY_CACHE = TTLCache[str, MarketHistoryResponse](max_size=256, ttl_seconds=60 * 30)


@router.get("/market/tickers", response_model=TickersResponse)
def list_tickers() -> TickersResponse:
    return TickersResponse(tickers=_UNIVERSE)


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

    period = _RANGE_PERIOD.get(range, "6mo")
    try:
        hist: pd.DataFrame = await run_in_threadpool(
            lambda: yf.Ticker(ticker).history(period=period, auto_adjust=True)
        )
    except Exception as exc:  # noqa: BLE001
        logger.error("market_history failed", extra={"ticker": ticker, "error": str(exc)})
        raise HTTPException(status_code=502, detail="Price provider unavailable") from exc

    if hist is None or hist.empty or "Close" not in hist:
        raise HTTPException(status_code=404, detail=f"No price history for {ticker}")

    points = [
        MarketHistoryPoint(date=idx.date(), close=float(row["Close"]))
        for idx, row in hist.iterrows()
        if pd.notna(row["Close"])
    ]
    response = MarketHistoryResponse(ticker=ticker, points=points)
    _HISTORY_CACHE.set(cache_key, response)
    logger.info("market_history", extra={"ticker": ticker, "range": range, "points": len(points)})
    return response
