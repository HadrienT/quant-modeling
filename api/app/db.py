"""Read access to the local Postgres filled by the `data-ingest` service.

Market data no longer comes from live yfinance / FRED calls: `data-ingest`
(a separate self-hosted service) writes it to a local Postgres on a schedule.
This module is read-only — it never creates tables or writes rows.

Tables it reads:
  prices.sp500_daily          (date, ticker, open, high, low, close, volume)
  macro.fred_series_latest    (series_id, date, value)  — current vintage view

If PGHOST is unset or the store is unreachable, callers get a clear 503 rather
than a silent fallback to a different data source.
"""

from __future__ import annotations

import os
from contextlib import contextmanager
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
    pool = ConnectionPool(_dsn(), min_size=1, max_size=4, open=False)
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
