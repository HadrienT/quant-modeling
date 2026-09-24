"""
Market data for pricing against the maintainer's own database -- never Yahoo.

Everything here reads what data-ingest (~/data-ingest) has already stored in
Postgres: option-chain snapshots, closes, dividend yields. If something is
missing or too old, the answer is a clear error naming what is missing and
where it should come from, not a fallback to a live fetch. This module
deliberately does not import yfinance at all, so "no Yahoo call" is a
structural property (tests/test_market_snapshot.py asserts it) rather than a
convention.

A stored snapshot is a *market date*: the surface's maturity axis is
measured from the day the chain was captured. So the effective valuation
date of a priced script is that snapshot date, and the caller must use it.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from datetime import date
from typing import Dict, List, Optional

import quantmodeling as qm

from . import db
from .audit.payloads import MarketInputStatus
from .telemetry import tracer
from .valuation import record_market_input

#: A valuation date may sit this many days after the snapshot it prices
#: against: a weekend plus a holiday. Beyond that the surface is stale
#: relative to the date being valued, and pricing on it would misstate the
#: market rather than approximate it.
MAX_SNAPSHOT_GAP_DAYS = 4

#: How far the close may trail the snapshot before it is refused outright
#: (matches db._MAX_PRICE_AGE_DAYS, the pipeline's existing rule)...
MAX_SPOT_GAP_DAYS = 7
#: ...and the gap beyond which it is accepted but reported: a weekend is
#: normal, more than that means spot and chain describe different markets.
SPOT_GAP_WARN_DAYS = 3

#: Same rule as db._MAX_DIVIDEND_AGE_DAYS: a trailing yield moves slowly.
MAX_DIVIDEND_GAP_DAYS = 30


class MarketDataUnavailable(RuntimeError):
    """The database lacks (or holds too stale) what pricing needs."""


@dataclass
class LocalVolMarket:
    ticker: str
    valuation_date: date  # the snapshot date: the market date being priced
    spot: float
    dividend: float
    K_grid: List[float]
    T_grid: List[float]
    sigma_loc_flat: List[float]
    warnings: List[Dict[str, str]] = field(default_factory=list)


def _ttm(expiry: date, as_of: date) -> float:
    return max((expiry - as_of).days, 0) / 365.0


def _quote(r: "db.OptionChainRow", ttm: float) -> "qm.RawOptionQuote":
    q = qm.RawOptionQuote()
    q.strike = r.strike
    q.ttm = ttm
    q.is_call = r.option_type == "call"
    q.bid = r.bid or 0.0
    q.ask = r.ask or 0.0
    q.last = r.last_price or 0.0
    q.volume = r.volume
    q.open_interest = r.open_interest
    iv = r.implied_volatility
    has_iv = iv is not None and iv == iv and iv > 0  # NaN-safe
    q.implied_vol = float(iv) if has_iv else 0.0
    q.has_iv = has_iv
    return q


def _chain_row_key(r: "db.OptionChainRow") -> tuple:
    """The fields of a stored quote that the calibration reads: what the
    valuation record hashes to detect a revised chain."""
    return (
        r.expiry.isoformat(),
        r.option_type,
        r.strike,
        r.bid,
        r.ask,
        r.last_price,
        r.volume,
        r.open_interest,
        r.implied_volatility,
    )


def _store(fn, *args):
    try:
        return fn(*args)
    except db.StoreUnavailable as exc:
        raise MarketDataUnavailable(
            f"the market database is unavailable ({exc}); no live fallback is used"
        ) from exc


def local_vol_market(
    ticker: str, rate: float, valuation_date: date
) -> LocalVolMarket:
    """Stored chain + close + dividend yield -> calibrated Dupire grid."""
    with tracer.start_as_current_span(
        "market_snapshot.load", attributes={"qm.valuation_date": str(valuation_date)}
    ):
        return _local_vol_market(ticker, rate, valuation_date)


def _local_vol_market(
    ticker: str, rate: float, valuation_date: date
) -> LocalVolMarket:
    ticker = ticker.upper().strip()

    snap = _store(db.options_snapshot_date_on_or_before, ticker, valuation_date)
    if snap is None:
        raise MarketDataUnavailable(
            f"no option-chain snapshot for '{ticker}' on or before "
            f"{valuation_date.isoformat()} in the database. data-ingest's "
            "options-chain-snapshot source tracks a fixed universe "
            "(OPTIONS_CHAIN_TICKERS); a ticker outside it has no stored surface."
        )
    gap = (valuation_date - snap).days
    if gap > MAX_SNAPSHOT_GAP_DAYS:
        raise MarketDataUnavailable(
            f"the latest stored option-chain snapshot for '{ticker}' is "
            f"{snap.isoformat()}, {gap} days before the valuation date "
            f"{valuation_date.isoformat()} (at most {MAX_SNAPSHOT_GAP_DAYS} "
            "allowed). Refresh it with `ingest run options-chain-snapshot`, "
            f"or value on the stored market date: valuation_date={snap.isoformat()}."
        )

    spot_row = _store(db.price_on_or_before, ticker, snap)
    if spot_row is None or (snap - spot_row[0]).days > MAX_SPOT_GAP_DAYS:
        have = f"latest is {spot_row[0].isoformat()}" if spot_row else "none stored"
        raise MarketDataUnavailable(
            f"no close for '{ticker}' within {MAX_SPOT_GAP_DAYS} days of the "
            f"snapshot {snap.isoformat()} in prices.sp500_daily ({have}). "
            "Refresh it with `ingest run sp500-prices`."
        )
    spot_date, spot = spot_row

    div_row = _store(db.dividend_yield_on_or_before, ticker, snap)
    if div_row is None or (snap - div_row[0]).days > MAX_DIVIDEND_GAP_DAYS:
        raise MarketDataUnavailable(
            f"no recent dividend yield for '{ticker}' in prices.dividend_yields "
            "(a non-payer is stored as 0.0, so a missing row means it was never "
            "ingested: dividend-yields covers the options-chain default universe "
            "only). No live fallback is used."
        )
    dividend = div_row[1]

    rows = _store(db.options_chain_snapshot, ticker, snap)
    quotes = []
    for r in rows:
        ttm = _ttm(r.expiry, snap)
        if ttm > 0:
            quotes.append(_quote(r, ttm))
    if not quotes:
        raise MarketDataUnavailable(
            f"the stored snapshot of '{ticker}' on {snap.isoformat()} has no "
            "unexpired quotes"
        )

    # What the valuation record keeps of each input (blueprint WP 18e): its
    # date, its source table and a hash of the value, so a replay can tell a
    # revised datum from a code change. The spot is `stale` past the gap at
    # which a warning is raised below — the same rule, not a second one.
    spot_status = (
        MarketInputStatus.STALE
        if (snap - spot_date).days > SPOT_GAP_WARN_DAYS
        else MarketInputStatus.OBSERVED
    )
    record_market_input(
        f"spot:{ticker}", "db:prices.sp500_daily", spot_date.isoformat(), spot_status, spot
    )
    record_market_input(
        f"dividend:{ticker}",
        "db:prices.dividend_yields",
        div_row[0].isoformat(),
        MarketInputStatus.OBSERVED,
        dividend,
    )
    record_market_input(
        f"option_chain:{ticker}",
        "db:options.chain_snapshot",
        snap.isoformat(),
        MarketInputStatus.OBSERVED,
        [_chain_row_key(r) for r in rows],
    )

    result = qm.calibrate_vol_surface(
        quotes, spot, rate, dividend, -0.6, 0.6, 100, 50,
        cleaning_params=qm.CleaningParams(),
    )

    warnings: List[Dict[str, str]] = []
    if snap != valuation_date:
        warnings.append(
            {
                "code": "market_date_shifted",
                "severity": "info",
                "message": (
                    f"Priced on the stored market date {snap.isoformat()} "
                    f"(requested {valuation_date.isoformat()}): the latest "
                    "option-chain snapshot on or before it."
                ),
            }
        )
    if (snap - spot_date).days > SPOT_GAP_WARN_DAYS:
        warnings.append(
            {
                "code": "stale_spot",
                "severity": "warning",
                "message": (
                    f"Spot is the {spot_date.isoformat()} close but the option "
                    f"chain is from {snap.isoformat()}: spot and surface describe "
                    "different days, which shifts the smile in moneyness. "
                    "The database's prices are behind its option snapshots."
                ),
            }
        )

    return LocalVolMarket(
        ticker=ticker,
        valuation_date=snap,
        spot=spot,
        dividend=dividend,
        K_grid=result["K_grid"],
        T_grid=result["T_grid"],
        sigma_loc_flat=result["sigma_loc_flat"],
        warnings=warnings,
    )
