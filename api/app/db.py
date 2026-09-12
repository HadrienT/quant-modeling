"""Read access to the local Postgres filled by the `data-ingest` service.

Market data no longer comes from live yfinance / FRED calls: `data-ingest`
(a separate self-hosted service) writes it to a local Postgres on a schedule.
This module is read-only, with one narrow, explicit exception (see
`cache_option_chain_snapshot` at the bottom): it never creates tables, and
callers should prefer reading over writing wherever the daily-scheduled
tables already cover what they need.

Tables it reads:
  prices.sp500_daily          (date, ticker, open, high, low, close, volume)
  prices.dividend_yields      (date, ticker, trailing_yield)
  macro.fred_series_latest    (series_id, date, value)  — current vintage view
  options.chain_snapshot      (date, ticker, expiry, option_type, strike,
                                bid, ask, last_price, volume, open_interest,
                                implied_volatility)

If PGHOST is unset or the store is unreachable, callers get a clear 503 rather
than a silent fallback to a different data source.
"""

from __future__ import annotations

import os
from contextlib import contextmanager
from dataclasses import dataclass
from datetime import date
from functools import lru_cache
from typing import Iterator, List, Optional, Sequence, Tuple

import pandas as pd

try:  # psycopg is optional at import time so the OpenAPI dump / tests still load
    import psycopg
    from psycopg_pool import ConnectionPool
except Exception:  # pragma: no cover
    psycopg = None  # type: ignore
    ConnectionPool = None  # type: ignore


def _dsn() -> str:
    host = os.getenv("PGHOST", "")
    if not host:
        raise RuntimeError(
            "PGHOST is not set — the data-ingest Postgres is not configured"
        )
    port = os.getenv("PGPORT", "5432")
    db = os.getenv("PGDATABASE", "dataingest")
    user = os.getenv("PGUSER", "dataingest")
    pw = os.getenv("PGPASSWORD", "")
    pw_part = f" password={pw}" if pw else ""
    return f"host={host} port={port} dbname={db} user={user}{pw_part} connect_timeout=5"


@lru_cache(maxsize=1)
def _pool() -> "ConnectionPool":
    if psycopg is None or ConnectionPool is None:
        raise RuntimeError("psycopg is not installed")
    # timeout: fail a checkout in 10s (→ 503) rather than hang a request;
    # check: validate a pooled connection before handing it out.
    pool = ConnectionPool(
        _dsn(),
        min_size=1,
        max_size=4,
        timeout=10.0,
        max_waiting=8,
        check=ConnectionPool.check_connection,
        open=False,
    )
    pool.open()
    return pool


def close_pool() -> None:
    """Called on API shutdown."""
    if _pool.cache_info().currsize:
        _pool().close()
        _pool.cache_clear()


class StoreUnavailable(RuntimeError):
    """Raised when the ingest Postgres cannot be reached — surfaced as 503."""


@contextmanager
def _cursor() -> Iterator["psycopg.Cursor"]:
    try:
        with _pool().connection() as conn, conn.cursor() as cur:
            yield cur
    except Exception as exc:  # noqa: BLE001
        raise StoreUnavailable(str(exc)) from exc


# ── prices.sp500_daily ───────────────────────────────────────────────────────


def sp500_tickers() -> List[str]:
    with _cursor() as cur:
        cur.execute("SELECT DISTINCT ticker FROM prices.sp500_daily ORDER BY ticker")
        return [r[0] for r in cur.fetchall()]


def price_history(
    ticker: str, since: Optional[date] = None
) -> List[Tuple[date, float]]:
    sql = "SELECT date, close FROM prices.sp500_daily WHERE ticker = %s"
    params: list = [ticker.upper()]
    if since is not None:
        sql += " AND date >= %s"
        params.append(since)
    sql += " AND close IS NOT NULL ORDER BY date ASC"
    with _cursor() as cur:
        cur.execute(sql, params)
        return [(r[0], float(r[1])) for r in cur.fetchall()]


def prices_wide(tickers: Sequence[str], since: Optional[date] = None) -> pd.DataFrame:
    """date-indexed, one column per ticker, adjusted close."""
    sql = (
        "SELECT date, ticker, close FROM prices.sp500_daily "
        "WHERE ticker = ANY(%s) AND close IS NOT NULL"
    )
    params: list = [list({t.upper() for t in tickers})]
    if since is not None:
        sql += " AND date >= %s"
        params.append(since)
    sql += " ORDER BY date ASC"
    with _cursor() as cur:
        cur.execute(sql, params)
        rows = cur.fetchall()
    if not rows:
        return pd.DataFrame()
    df = pd.DataFrame(rows, columns=["date", "ticker", "close"])
    wide = df.pivot(index="date", columns="ticker", values="close")
    wide.index = pd.to_datetime(wide.index).tz_localize("UTC")
    return wide.sort_index()


# ── macro.fred_series_latest ─────────────────────────────────────────────────


def fred_series(series_id: str, since: Optional[date] = None) -> "pd.Series":
    sql = (
        "SELECT date, value FROM macro.fred_series_latest "
        "WHERE series_id = %s AND value IS NOT NULL"
    )
    params: list = [series_id]
    if since is not None:
        sql += " AND date >= %s"
        params.append(since)
    sql += " ORDER BY date ASC"
    with _cursor() as cur:
        cur.execute(sql, params)
        rows = cur.fetchall()
    if not rows:
        return pd.Series(dtype="float64")
    s = pd.Series(
        [float(v) for _, v in rows],
        index=pd.to_datetime([d for d, _ in rows]),
        name=series_id,
    )
    return s


def fred_latest_value(series_id: str) -> Optional[float]:
    with _cursor() as cur:
        cur.execute(
            "SELECT value FROM macro.fred_series_latest "
            "WHERE series_id = %s AND value IS NOT NULL "
            "ORDER BY date DESC LIMIT 1",
            (series_id,),
        )
        row = cur.fetchone()
        return float(row[0]) if row else None


# ── options.chain_snapshot ───────────────────────────────────────────────────


@dataclass(frozen=True, slots=True)
class OptionChainRow:
    """One row of options.chain_snapshot, as written by data-ingest's
    options-chain-snapshot source — see ~/data-ingest's README for the
    schema and why history can only be built by running that source daily,
    never backfilled from yfinance."""

    snapshot_date: date
    expiry: date
    option_type: str  # "call" or "put"
    strike: float
    bid: Optional[float]
    ask: Optional[float]
    last_price: Optional[float]
    volume: int
    open_interest: int
    implied_volatility: Optional[float]


def latest_options_snapshot_date(ticker: str) -> Optional[date]:
    """The most recent date data-ingest captured a chain for this ticker, if any."""
    with _cursor() as cur:
        cur.execute(
            "SELECT MAX(date) FROM options.chain_snapshot WHERE ticker = %s",
            (ticker.upper(),),
        )
        row = cur.fetchone()
        return row[0] if row and row[0] is not None else None


def options_chain_snapshot(
    ticker: str, as_of: Optional[date] = None
) -> List[OptionChainRow]:
    """One ticker's full chain on `as_of` (or its latest stored date, if
    omitted). Empty list if data-ingest has never captured this ticker —
    options-chain-snapshot tracks a small fixed universe
    (OPTIONS_CHAIN_TICKERS), not every ticker sp500-prices does, so an empty
    result here is routine, not an error; callers fall back to a live fetch.
    """
    snapshot_date = as_of or latest_options_snapshot_date(ticker)
    if snapshot_date is None:
        return []

    with _cursor() as cur:
        cur.execute(
            "SELECT date, expiry, option_type, strike, bid, ask, last_price, "
            "volume, open_interest, implied_volatility "
            "FROM options.chain_snapshot WHERE ticker = %s AND date = %s "
            "ORDER BY expiry, option_type, strike",
            (ticker.upper(), snapshot_date),
        )
        rows = cur.fetchall()

    return [
        OptionChainRow(
            snapshot_date=r[0],
            expiry=r[1],
            option_type=r[2],
            strike=float(r[3]),
            bid=float(r[4]) if r[4] is not None else None,
            ask=float(r[5]) if r[5] is not None else None,
            last_price=float(r[6]) if r[6] is not None else None,
            volume=int(r[7]),
            open_interest=int(r[8]),
            implied_volatility=float(r[9]) if r[9] is not None else None,
        )
        for r in rows
    ]


# ── prices.sp500_daily / prices.dividend_yields, for the vol-surface pipeline ──

# A close or a trailing yield older than this is treated as absent rather
# than used: sp500-prices and dividend-yields both run on a Mon-Fri schedule,
# so a healthy pipeline never produces a gap this wide, and using a stale
# number silently would be worse than falling back to a live fetch.
_MAX_PRICE_AGE_DAYS = 7
_MAX_DIVIDEND_AGE_DAYS = 30


def latest_price(ticker: str) -> Optional[Tuple[date, float]]:
    """Most recent (date, close) from prices.sp500_daily, or None if there is
    none or it is too stale. sp500-prices' tracked universe (519 tickers,
    including the index ETFs options-chain-snapshot uses) already covers
    everything the vol-surface pipeline's default ticker set needs."""
    with _cursor() as cur:
        cur.execute(
            "SELECT date, close FROM prices.sp500_daily "
            "WHERE ticker = %s AND close IS NOT NULL ORDER BY date DESC LIMIT 1",
            (ticker.upper(),),
        )
        row = cur.fetchone()
    if not row:
        return None
    as_of, price = row[0], float(row[1])
    if (date.today() - as_of).days > _MAX_PRICE_AGE_DAYS:
        return None
    return as_of, price


def latest_dividend_yield(ticker: str) -> Optional[Tuple[date, float]]:
    """Most recent (date, trailing_yield) from prices.dividend_yields, or
    None if there is none or it is too stale."""
    with _cursor() as cur:
        cur.execute(
            "SELECT date, trailing_yield FROM prices.dividend_yields "
            "WHERE ticker = %s AND trailing_yield IS NOT NULL ORDER BY date DESC LIMIT 1",
            (ticker.upper(),),
        )
        row = cur.fetchone()
    if not row:
        return None
    as_of, value = row[0], float(row[1])
    if (date.today() - as_of).days > _MAX_DIVIDEND_AGE_DAYS:
        return None
    return as_of, value


# ── The one write path: caching a live option-chain fetch ──────────────────


def cache_option_chain_snapshot(
    ticker: str, snapshot_date: date, rows: Sequence[dict]
) -> int:
    """Upsert quotes just fetched live from yfinance into
    options.chain_snapshot -- this module's one deliberate exception to
    read-only.

    Why this table and not the others: an option chain is the one fetch in
    this pipeline that is both expensive (one call per expiration, on top of
    the list call) and rate-limited (an unofficial endpoint), so a second
    request for the same ticker on the same day hitting yfinance again is a
    real, avoidable cost. A spot price or a dividend yield is a single cheap
    call, and its tracked universe (sp500-prices, dividend-yields) already
    covers what the vol-surface pipeline needs daily, so there is nothing
    worth caching there beyond what those two sources already provide on
    schedule.

    Same schema, same upsert semantics data-ingest's own scheduled run would
    use (ON CONFLICT on the declared primary key): if options-chain-snapshot
    is ever pointed at this ticker too, its next run simply overwrites these
    rows, harmlessly. If the table does not exist yet (a fresh deployment
    where data-ingest has never run), this raises StoreUnavailable like any
    other unreachable-store case, and the caller treats caching as best-effort.
    """
    if not rows:
        return 0

    payload = [
        {
            "date": snapshot_date,
            "ticker": ticker.upper(),
            "expiry": r["expiry"],
            "option_type": r["option_type"],
            "strike": r["strike"],
            "bid": r.get("bid"),
            "ask": r.get("ask"),
            "last_price": r.get("last_price"),
            "volume": r.get("volume", 0),
            "open_interest": r.get("open_interest", 0),
            "implied_volatility": r.get("implied_volatility"),
        }
        for r in rows
    ]

    with _cursor() as cur:
        cur.executemany(
            """
            INSERT INTO options.chain_snapshot
                (date, ticker, expiry, option_type, strike, bid, ask,
                 last_price, volume, open_interest, implied_volatility)
            VALUES
                (%(date)s, %(ticker)s, %(expiry)s, %(option_type)s, %(strike)s,
                 %(bid)s, %(ask)s, %(last_price)s, %(volume)s, %(open_interest)s,
                 %(implied_volatility)s)
            ON CONFLICT (date, ticker, expiry, option_type, strike) DO UPDATE SET
                bid = EXCLUDED.bid,
                ask = EXCLUDED.ask,
                last_price = EXCLUDED.last_price,
                volume = EXCLUDED.volume,
                open_interest = EXCLUDED.open_interest,
                implied_volatility = EXCLUDED.implied_volatility
            """,
            payload,
        )
    return len(payload)
