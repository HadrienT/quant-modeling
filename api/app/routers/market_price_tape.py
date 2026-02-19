from datetime import datetime
from typing import Dict, List

from fastapi import APIRouter, Depends, HTTPException, Query
from google.cloud import bigquery
from pydantic import BaseModel

from ..cache import TTLCache
from ..dependencies import bq_project_id, bq_table_ref, require_api_key
from ..logging_utils import get_logger
from ..request_context import set_cache_hit
from ..schemas import MarketHistoryPoint, MarketHistoryResponse

router = APIRouter()
logger = get_logger()


class TickersResponse(BaseModel):
    tickers: List[str]


_RANGE_LIMITS: Dict[str, int] = {
    "1M": 21,
    "3M": 63,
    "6M": 126,
    "1Y": 252,
    "2Y": 504,
}


def _get_range_limit(range_key: str) -> int:
    if range_key == "YTD":
        today = datetime.now()
        start_of_year = datetime(today.year, 1, 1)
        days_elapsed = (today - start_of_year).days
        return max(1, int(days_elapsed * 0.7))
    return _RANGE_LIMITS.get(range_key, 126)


_HISTORY_CACHE = TTLCache[str, MarketHistoryResponse](max_size=256, ttl_seconds=60 * 30)
_TICKERS_CACHE = TTLCache[str, TickersResponse](max_size=1, ttl_seconds=60 * 60 * 24)


@router.get("/market/tickers", response_model=TickersResponse, dependencies=[Depends(require_api_key)])
def list_tickers() -> TickersResponse:
    cache_key = "all_tickers"
    cached = _TICKERS_CACHE.get(cache_key)
    if cached:
        set_cache_hit()
        logger.info("list_tickers cache_hit", extra={"count": len(cached.tickers)})
        return cached

    project_id = bq_project_id()
    try:
        client = bigquery.Client(project=project_id)
        query = f"""
        SELECT DISTINCT Ticker
        FROM `{bq_table_ref()}`
        ORDER BY Ticker ASC
        """
        rows = list(client.query(query).result())
        tickers = [row["Ticker"] for row in rows if row["Ticker"]]
        response = TickersResponse(tickers=tickers)
        _TICKERS_CACHE.set(cache_key, response)
        logger.info("list_tickers cache_miss", extra={"count": len(tickers)})
        return response
    except Exception as exc:
        logger.error("list_tickers failed", extra={"error": str(exc)})
        raise HTTPException(status_code=500, detail="Failed to fetch tickers from BigQuery")


@router.get("/market/prices/history", response_model=MarketHistoryResponse, dependencies=[Depends(require_api_key)])
def market_history(
    ticker: str = Query(..., min_length=1),
    range: str = Query("6M", pattern="^(1M|3M|6M|YTD|1Y|2Y)$"),
) -> MarketHistoryResponse:
    limit = _get_range_limit(range)
    cache_key = ticker

    cached = _HISTORY_CACHE.get(cache_key)
    if cached and len(cached.points) >= limit:
        set_cache_hit()
        sliced = cached.points[-limit:]
        logger.info(
            "market_history cache_hit",
            extra={"ticker": ticker, "range": range, "points": len(sliced)},
        )
        return MarketHistoryResponse(ticker=ticker, points=sliced)

    max_limit = _RANGE_LIMITS["2Y"]
    project_id = bq_project_id()
    try:
        client = bigquery.Client(project=project_id)
        query = f"""
        SELECT Date, Close
        FROM `{bq_table_ref()}`
        WHERE Ticker = @ticker
        ORDER BY Date DESC
        LIMIT @limit
        """
        job_config = bigquery.QueryJobConfig(
            query_parameters=[
                bigquery.ScalarQueryParameter("ticker", "STRING", ticker),
                bigquery.ScalarQueryParameter("limit", "INT64", max_limit),
            ]
        )
        rows = list(client.query(query, job_config=job_config).result())
        full_points: List[MarketHistoryPoint] = [
            MarketHistoryPoint(date=row["Date"], close=float(row["Close"]))
            for row in reversed(rows)
            if row["Close"] is not None
        ]

        response = MarketHistoryResponse(ticker=ticker, points=full_points)
        _HISTORY_CACHE.set(cache_key, response)

        sliced = full_points[-limit:]
        logger.info(
            "market_history cache_miss",
            extra={"ticker": ticker, "range": range, "cached_points": len(full_points), "returned_points": len(sliced)},
        )
        return MarketHistoryResponse(ticker=ticker, points=sliced)
    except Exception as exc:
        logger.error("market_history failed", extra={"ticker": ticker, "range": range, "error": str(exc)})
        raise HTTPException(status_code=500, detail="BigQuery query failed")
