"""What a backtest reads, in the shape the `quantbacktest` wheel takes
(blueprint/wp/24-backtest.md, lot B8; ~/quant-backtest/docs/contract.md §3).

Reads and reshapes only: rows become tables with one row per date and one
column per ticker, a missing quote stays a NaN, and nothing is filled, adjusted
or computed here. The two conversions a backtest needs (an annual rate into
period rates, raw closes and events into total returns) are functions of the
wheel.

Missing data is an error, never a fallback: a ticker without a quote in the
window, or a window the short rate does not cover, raises `MissingData`.
"""

from __future__ import annotations

from dataclasses import dataclass
from datetime import date, timedelta
from typing import Sequence

import numpy as np

from .. import db
from ..cache import TTLCache

#: The total-return benchmark: SPY with its dividends (data-ingest README).
BENCHMARK = "SPY"
#: 3-month Treasury constant maturity, percent a year, as FRED publishes it.
CASH_SERIES = "DGS3MO"
#: Money-market day count of the US dollar.
CASH_BASIS = 360.0
#: Kenneth French's datasets; `RF` is read from `ff3` whatever the dataset.
FACTOR_DATASETS = {
    "ff3": ("Mkt-RF", "SMB", "HML"),
    "ff5": ("Mkt-RF", "SMB", "HML", "RMW", "CMA"),
}

Events = tuple[np.ndarray, np.ndarray, np.ndarray]


class MissingData(ValueError):
    """A datum the run needs is not in the database. Shown as it is (422)."""


@dataclass(frozen=True)
class Market:
    """Bars as quoted for a list of tickers over a window. `dates` is the set
    of days on which at least one of them was quoted."""

    tickers: tuple[str, ...]
    dates: np.ndarray  # datetime64[D], strictly increasing
    open: np.ndarray  # days × tickers, NaN = no quote
    high: np.ndarray
    low: np.ndarray
    close: np.ndarray
    volume: np.ndarray
    dividends: Events  # (column, ex-date, amount per share)
    splits: Events  # (column, date, new shares per old)


def _events(rows: Sequence[tuple], column: dict[str, int]) -> Events:
    return (
        np.array([column[t] for t, _, _ in rows], dtype=np.int64),
        np.array([d for _, d, _ in rows], dtype="datetime64[D]"),
        np.array([v for _, _, v in rows], dtype=np.float64),
    )


# Keyed by tickers AND dates: two windows never share their prices (finding 1
# of the old page, an acceptance criterion of WP 24).
_markets: TTLCache[tuple, Market] = TTLCache(max_size=32, ttl_seconds=15 * 60)


def market(tickers: Sequence[str], start: date, end: date) -> Market:
    tickers = tuple(tickers)
    key = (tickers, start, end)
    cached = _markets.get(key)
    if cached is not None:
        return cached

    bars = db.raw_daily_bars(tickers, start, end)
    quoted = {row[1] for row in bars}
    absent = [t for t in tickers if t not in quoted]
    if absent:
        raise MissingData(
            f"No quote for {', '.join(absent)} between {start} and {end}."
        )

    column = {t: i for i, t in enumerate(tickers)}
    dates = np.array(sorted({row[0] for row in bars}), dtype="datetime64[D]")
    row_of = {d: i for i, d in enumerate(dates.astype(object))}
    fields = np.full((5, len(dates), len(tickers)), np.nan)
    for day, ticker, *values in bars:
        fields[:, row_of[day], column[ticker]] = [
            np.nan if v is None else v for v in values
        ]
    result = Market(
        tickers=tickers,
        dates=dates,
        open=fields[0],
        high=fields[1],
        low=fields[2],
        close=fields[3],
        volume=fields[4],
        dividends=_events(db.dividends(tickers, start, end), column),
        splits=_events(db.splits(tickers, start, end), column),
    )
    _markets.set(key, result)
    return result


def annual_cash_rate(dates: np.ndarray) -> np.ndarray:
    """The 3-month Treasury rate known at each date, as a decimal a year: the
    last value published on or before it. FRED has no value on a bond-market
    holiday; the rate known that day is then the previous one."""
    start = dates[0].astype(object) - timedelta(days=15)
    rows = db.fred_first_vintage(CASH_SERIES, start, dates[-1].astype(object))
    if not rows:
        raise MissingData(f"No {CASH_SERIES} rate before {dates[0]}.")
    published = np.array([d for d, _ in rows], dtype="datetime64[D]")
    values = np.array([v for _, v in rows]) / 100.0  # percent → decimal
    index = np.searchsorted(published, dates, side="right") - 1
    if index[0] < 0:
        raise MissingData(f"No {CASH_SERIES} rate on or before {dates[0]}.")
    return values[index]


@dataclass(frozen=True)
class Factors:
    dataset: str
    names: tuple[str, ...]
    dates: np.ndarray
    values: np.ndarray  # days × factors, decimal per day
    risk_free: np.ndarray  # French's RF, decimal per day


def factors(dataset: str, start: date, end: date) -> Factors:
    """One dataset at a time (`SMB` is not the same number in `ff3` and
    `ff5`), stored in percent and handed over as decimals."""
    names = FACTOR_DATASETS[dataset]
    table: dict[date, dict[str, float]] = {}
    for day, factor, value in db.french_first_vintage(dataset, start, end):
        table.setdefault(day, {})[factor] = value
    risk_free = {
        day: value
        for day, factor, value in db.french_first_vintage("ff3", start, end)
        if factor == "RF"
    }
    days = sorted(
        d for d, row in table.items() if all(n in row for n in names) and d in risk_free
    )
    if not days:
        raise MissingData(f"No {dataset} factors between {start} and {end}.")
    return Factors(
        dataset=dataset,
        names=names,
        dates=np.array(days, dtype="datetime64[D]"),
        values=np.array([[table[d][n] for n in names] for d in days]) / 100.0,
        risk_free=np.array([risk_free[d] for d in days]) / 100.0,
    )
