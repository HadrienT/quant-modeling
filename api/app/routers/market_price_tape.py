"""Price tape — read from the local Postgres filled by `data-ingest` (no cloud,
no live yfinance calls here).

Five equity markets: the S&P 500, and the CAC 40, DAX, FTSE 100 and Nikkei 225
(data-ingest's equity-universe and intl-equity-prices sources). Prices are in
each line's own currency, returned with the history; they are never converted.
"""

from dataclasses import asdict
from datetime import date, timedelta
from typing import Dict, List, Literal, Optional

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


class TickerInfo(BaseModel):
    ticker: str
    name: Optional[str] = None
    kind: Literal["index", "equity"]
    currency: str


class TickersResponse(BaseModel):
    tickers: List[str]
    #: With ?market=: the market's members, the index first, with name and
    #: currency. Empty without it (the historical, market-less list).
    members: List[TickerInfo] = []


MarketId = Literal["SP500", "CAC40", "DAX", "FTSE100", "NIKKEI225"]


class MarketInfo(BaseModel):
    id: MarketId
    name: str
    currency: str
    members: int
    as_of: Optional[date] = None
    has_options: bool
    note: Optional[str] = None


class MarketsResponse(BaseModel):
    markets: List[MarketInfo]


#: Display name, index currency, and whether option chains exist — the
#: Volatility tab (SVI surface, Dupire) needs them, and Yahoo publishes them
#: for US listings only.
_NO_OPTIONS = (
    "No free option data: Yahoo Finance publishes option chains for US "
    "listings only, so this market has prices but no volatility surface."
)
MARKETS: Dict[str, tuple[str, str, bool, Optional[str]]] = {
    "SP500": (
        "S&P 500",
        "USD",
        True,
        "Option chains are stored daily for a fixed set of liquid names and "
        "index ETFs (data-ingest's options-chain-snapshot), not every member.",
    ),
    "CAC40": ("CAC 40", "EUR", False, _NO_OPTIONS),
    "DAX": ("DAX", "EUR", False, _NO_OPTIONS),
    "FTSE100": (
        "FTSE 100",
        "GBP",
        False,
        _NO_OPTIONS + " London quotes in pence are shown in pounds; a few "
        "members trade in USD or EUR.",
    ),
    "NIKKEI225": ("Nikkei 225", "JPY", False, _NO_OPTIONS),
}


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
_TICKERS_CACHE = TTLCache[str, TickersResponse](max_size=8, ttl_seconds=60 * 60)
_MARKETS_CACHE = TTLCache[str, MarketsResponse](max_size=1, ttl_seconds=60 * 60)

# Liquid index-tracking ETFs -- not S&P 500 constituents themselves (so
# prices.sp500_daily, and hence db.sp500_tickers, doesn't carry them), but
# exactly what a vol surface looks cleanest on: far deeper open interest and
# tighter spreads than any single name, at every strike and maturity that
# matters. The raw indices themselves (^GSPC/^SPX, ^NDX, ^RUT) aren't
# offered here because yfinance does not expose an options chain for a raw
# index ticker the way it does for a tradable one -- these ETFs are the
# standard practical proxy. Matches data-ingest's own options_chain.py
# DEFAULT_TICKERS index names.
_INDEX_ETF_PROXIES = ["SPY", "QQQ", "IWM", "DIA"]


@router.get("/market/markets", response_model=MarketsResponse)
async def list_markets() -> MarketsResponse:
    cached = _MARKETS_CACHE.get("all")
    if cached:
        set_cache_hit()
        return cached
    try:
        found = {m: (n, d) for m, n, d in await run_in_threadpool(db.equity_markets)}
    except db.StoreUnavailable as exc:
        raise HTTPException(
            status_code=503, detail="Market data store unavailable"
        ) from exc
    response = MarketsResponse(
        markets=[
            MarketInfo(
                id=mid,
                name=name,
                currency=ccy,
                members=found[mid][0],
                as_of=found[mid][1],
                has_options=has_options,
                note=note,
            )
            for mid, (name, ccy, has_options, note) in MARKETS.items()
            if mid in found
        ]
    )
    _MARKETS_CACHE.set("all", response)
    return response


@router.get("/market/tickers", response_model=TickersResponse)
async def list_tickers(market: Optional[MarketId] = Query(None)) -> TickersResponse:
    if market is not None:
        return await _market_tickers(market)
    cached = _TICKERS_CACHE.get("all")
    if cached:
        set_cache_hit()
        return cached
    try:
        sp500 = await run_in_threadpool(db.sp500_tickers)
    except db.StoreUnavailable as exc:
        # Degrade to the static ETF list rather than failing outright --
        # those don't need the price-tape store at all, and are exactly
        # what you'd reach for first if the store were down anyway.
        logger.warning(
            "list_tickers: store unavailable, falling back to index ETFs only",
            extra={"error": str(exc)},
        )
        sp500 = []
    tickers = sorted(set(sp500) | set(_INDEX_ETF_PROXIES))
    response = TickersResponse(tickers=tickers)
    _TICKERS_CACHE.set("all", response)
    return response


async def _market_tickers(market: str) -> TickersResponse:
    cached = _TICKERS_CACHE.get(market)
    if cached:
        set_cache_hit()
        return cached
    try:
        members = await run_in_threadpool(db.market_members, market)
    except db.StoreUnavailable as exc:
        raise HTTPException(
            status_code=503, detail="Market data store unavailable"
        ) from exc
    if not members:
        raise HTTPException(status_code=404, detail=f"No members for {market}")
    extra = _INDEX_ETF_PROXIES if market == "SP500" else []
    infos = [TickerInfo(**asdict(m)) for m in members] + [
        TickerInfo(ticker=t, name=f"{t} (index ETF)", kind="equity", currency="USD")
        for t in extra
    ]
    response = TickersResponse(tickers=[i.ticker for i in infos], members=infos)
    _TICKERS_CACHE.set(market, response)
    return response


@router.get("/market/prices/history", response_model=MarketHistoryResponse)
async def market_history(
    ticker: str = Query(..., min_length=1, max_length=16),
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
        currency = await run_in_threadpool(db.ticker_currency, ticker)
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
        # Outside the universe: an S&P 500 name or index ETF, both US listings.
        currency=currency or "USD",
        points=[MarketHistoryPoint(date=d, close=c) for d, c in rows],
    )
    _HISTORY_CACHE.set(cache_key, response)
    logger.info(
        "market_history", extra={"ticker": ticker, "range": range, "points": len(rows)}
    )
    return response
