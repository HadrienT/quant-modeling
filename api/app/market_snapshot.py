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

import hashlib
import json
import os
from dataclasses import dataclass, field
from datetime import date
from typing import Dict, List

import quantmodeling as qm

from . import chain_iv, db
from .audit.payloads import MarketInputStatus
from .compute_timing import phase
from .keyed_cache import KeyedCache
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

#: The log-moneyness band requested for the Dupire grid (each maturity is
#: then computed on its own quoted band within it, WP 26 §9.3) and its size.
K_MIN, K_MAX = -0.6, 0.6
N_STRIKES, N_MATURITIES = 100, 50


#: Calibrated surfaces (SVI slices + Dupire grid, about a second each), by a
#: digest of their inputs.
_surfaces: KeyedCache[dict] = KeyedCache(32)


class MarketDataUnavailable(RuntimeError):
    """The database lacks (or holds too stale) what pricing needs."""


#: Which option data the surfaces are built from (WP 31, decision D5):
#: `legacy`, options.chain_snapshot with Yahoo's implied vols and today's
#: cleaning, or `chain_iv`, the quotes data-ingest cleaned at ingestion
#: (options.chain_iv: our own implied vols, OTM, the market's forwards).
#: `legacy` until the maintainer switches; a rollback is this variable.
OPTION_DATA_ENV = "QM_OPTION_DATA"
OPTION_DATA = ("legacy", "chain_iv")


def option_data() -> str:
    value = os.environ.get(OPTION_DATA_ENV, "legacy").strip().lower() or "legacy"
    if value not in OPTION_DATA:
        raise ValueError(
            f"{OPTION_DATA_ENV}={value!r}: expected one of {', '.join(OPTION_DATA)}"
        )
    return value


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
    #: The calibrated SVI slices the Dupire grid was built from (ttm, a, b,
    #: rho, m, sigma, rmse, converged...): an implied-vol surface in its own
    #: right, k = ln(K / F_T) at the snapshot's spot, rate and dividend.
    svi_slices: List[Dict[str, float]] = field(default_factory=list)
    #: Flat rate the calibration's forwards used.
    rate: float = 0.0
    #: The log-moneyness range and grid size the Dupire grid was built on:
    #: what the superbucket (market_vega.py) rebuilds it with.
    k_min: float = -0.6
    k_max: float = 0.6
    n_strikes: int = 100
    n_maturities: int = 50
    #: The implied-vol surface the grid came from ("svi" or "essvi") and the
    #: eSSVI settings: the superbucket reruns that calibration.
    surface_model: str = "svi"
    essvi_forward_smoothing: float = 0.1
    essvi_min_forward_vol: float = 0.05
    #: The option data the surface came from (QM_OPTION_DATA).
    option_data: str = "legacy"

    def surface_slices(self) -> List[Dict[str, float]]:
        """The SVI slices every mark reads: see surface_slices()."""
        return surface_slices(self.svi_slices)


def surface_slices(slices: List[Dict[str, float]]) -> List[Dict[str, float]]:
    """The slices the Dupire grid is built from (`in_surface`, chosen by
    select_surface_slices in vol_surface_pipeline.hpp), sorted by maturity.

    Vanilla marks, the flat Black-Scholes vol of scripts and the Heston fit
    read these and only these, so a vanilla and a path-dependent product on
    the same underlying are marked on one surface (#119). A slice left out
    contradicts its neighbours (calendar arbitrage) or does not span the
    grid's strikes; reading it for vanillas alone made a 1y 115 % call 15.4
    vol on the smile against 4.4 on the local vol. Slices carrying no flag
    (built before the selection existed) are all kept.
    """
    kept = [s for s in slices if s.get("in_surface", True)]
    return sorted(kept, key=lambda s: s["ttm"])


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


#: Where the chain's fingerprint comes from: data-ingest's load registry, or
#: (no registry row) a digest of the rows read.
REGISTRY_SOURCE = "db:options.chain_snapshot_loads"
ROWS_SOURCE = "db:options.chain_snapshot"


@dataclass(frozen=True)
class _Chain:
    """A stored chain made ready for the calibration: its unexpired quotes
    (native objects, ~30 ms to build for SPY) and what identifies it."""

    quotes: list
    fingerprint: str
    source: str
    #: What the surface's reader should know about the chain (chain_iv: the
    #: proxy surface of D2, the discount's fallback).
    warnings: tuple = ()


#: Parsed chains by (ticker, snapshot date, registry checksum): ~1.2 MB per
#: loaded SPY market, so a bound well above the 10 tickers data-ingest tracks
#: still holds tens of megabytes at most. No time to live: the checksum is in
#: the key, so a rewritten snapshot is a new key, never a stale hit (#332).
_CHAIN_CACHE_SIZE = 24
_chains: KeyedCache[_Chain] = KeyedCache(_CHAIN_CACHE_SIZE)


def _rows_digest(rows: List["db.OptionChainRow"]) -> str:
    return hashlib.sha256(
        json.dumps([_chain_row_key(r) for r in rows]).encode()
    ).hexdigest()


def _read_chain(ticker: str, snap: date) -> tuple[list, list]:
    rows = read_store(db.options_chain_snapshot, ticker, snap)
    quotes = []
    for r in rows:
        ttm = _ttm(r.expiry, snap)
        if ttm > 0:
            quotes.append(_quote(r, ttm))
    return rows, quotes


def _chain(ticker: str, load: "db.ChainSnapshotLoad") -> _Chain:
    """The chain `load` names. With a registry row, read once per checksum
    and then served from memory; without one, read in full on every call
    (the path before the registry), so the cache never depends on it."""
    snap = load.snapshot_date
    if load.checksum is None:
        rows, quotes = _read_chain(ticker, snap)
        return _Chain(quotes, _rows_digest(rows), ROWS_SOURCE)

    def read() -> _Chain:
        rows, quotes = _read_chain(ticker, snap)
        # The registry row was read before the chain: if data-ingest rewrote
        # the snapshot in between, these rows are not the ones the checksum
        # names. Re-read it (one row, a miss only) and refuse to cache then.
        again = read_store(db.options_chain_load_on_or_before, ticker, snap)
        if (
            again is None
            or again.checksum != load.checksum
            or (load.row_count is not None and load.row_count != len(rows))
        ):
            raise _ChainChanged(_Chain(quotes, _rows_digest(rows), ROWS_SOURCE))
        return _Chain(quotes, load.checksum, REGISTRY_SOURCE)

    try:
        return _chains.get((ticker, snap, load.checksum), read)
    except _ChainChanged as changed:
        return changed.chain


class _ChainChanged(Exception):
    """The snapshot changed while it was read: use the rows read, uncached."""

    def __init__(self, chain: _Chain) -> None:
        super().__init__("option chain rewritten during the read")
        self.chain = chain


IV_REGISTRY_SOURCE = "db:options.chain_iv_loads"
IV_ROWS_SOURCE = "db:options.chain_iv"


def _iv_rows(ticker: str, load: "db.ChainIvLoad") -> tuple:
    """A computed chain's rows and their fingerprint (the registry's
    checksum, else a digest of the rows)."""
    rows = read_store(db.options_chain_iv, ticker, load.snapshot_date)
    if load.checksum:
        return rows, f"{load.checksum}:v{load.method_version}", IV_REGISTRY_SOURCE
    digest = hashlib.sha256(
        json.dumps([chain_iv.row_key(r) for r in rows]).encode()
    ).hexdigest()
    return rows, digest, IV_ROWS_SOURCE


def _iv_chain(ticker: str, load: "db.ChainIvLoad", spot: float) -> _Chain:
    """The surface's quotes from options.chain_iv. A ticker with a European
    proxy (D2: SPY on ^SPX) takes the proxy's quotes of the same day carried
    onto its own forwards, or its own quotes, with a warning, on a day the
    proxy has none."""
    snap = load.snapshot_date
    proxy = chain_iv.EUROPEAN_PROXY.get(ticker)
    proxy_load = (
        read_store(db.options_chain_iv_load_on_or_before, proxy, snap)
        if proxy
        else None
    )
    if proxy_load is not None and proxy_load.snapshot_date != snap:
        proxy_load = None

    def read() -> _Chain:
        rows, fingerprint, source = _iv_rows(ticker, load)
        warnings = []
        if load.curve_source == "fred":
            warnings.append(
                {
                    "code": "rate_fallback",
                    "severity": "info",
                    "message": (
                        f"The {snap.isoformat()} chain is discounted on FRED's "
                        "SOFR and Treasury yields: no European index chain was "
                        "stored that day to read the option market's own rate from."
                    ),
                }
            )
        if proxy_load is not None:
            proxy_rows, proxy_fp, _ = _iv_rows(proxy, proxy_load)
            expiries = read_store(db.options_chain_iv_expiries, ticker, snap)
            if expiries:
                quotes = chain_iv.carried_onto(
                    proxy_rows, chain_iv.CarryCurve.of(expiries, spot)
                )
                warnings.append(
                    {
                        "code": "european_proxy_surface",
                        "severity": "info",
                        "message": (
                            f"The surface is {proxy}'s European options of "
                            f"{snap.isoformat()}, carried onto {ticker}'s own "
                            "forwards: the same smile in ln(K/F), at "
                            f"{ticker}'s scale, with no early-exercise premium "
                            "to remove."
                        ),
                    }
                )
                return _Chain(
                    quotes,
                    hashlib.sha256(
                        json.dumps([fingerprint, proxy_fp, spot]).encode()
                    ).hexdigest(),
                    source,
                    tuple(warnings),
                )
        if proxy:
            warnings.append(
                {
                    "code": "own_american_surface",
                    "severity": "warning",
                    "message": (
                        f"No {proxy} chain is stored for {snap.isoformat()}: the "
                        f"surface is {ticker}'s own American options, "
                        "de-Americanized at ingestion, instead of the European "
                        "index surface."
                    ),
                }
            )
        quotes = [chain_iv.quote(r) for r in chain_iv.usable(rows)]
        return _Chain(quotes, fingerprint, source, tuple(warnings))

    if not load.checksum or (proxy_load is not None and not proxy_load.checksum):
        return read()
    key = (
        "chain_iv",
        ticker,
        snap,
        load.checksum,
        proxy_load.checksum if proxy_load is not None else None,
        spot,
    )
    return _chains.get(key, read)


def warm_chain(ticker: str, today: date) -> bool:
    """Loads a ticker's latest chain into memory before any request asks for
    it. True if it is now cached (False without a registry row). Under
    QM_OPTION_DATA=chain_iv the chain needs the spot it is carried onto, so
    the calibration warm-up (calibration_warmup.py) loads it instead."""
    if option_data() == "chain_iv":
        return False
    load = read_store(db.options_chain_load_on_or_before, ticker.upper(), today)
    if load is None or load.checksum is None:
        return False
    return _chain(ticker.upper(), load).source == REGISTRY_SOURCE


def clear_caches() -> None:
    """Forget every parsed chain and calibrated surface (tests)."""
    _chains.clear()
    _surfaces.clear()


def chain_quotes(ticker: str, rate: float, valuation_date: date) -> tuple:
    """The stored chain's quotes (from the cache when the registry names
    them), with what reading them needs: (quotes, spot, rate, dividend,
    snapshot date) -- for a precision target inferred from the market's
    spreads (precision.delta_sigma)."""
    m = local_vol_market(ticker, rate, valuation_date)
    ticker = ticker.upper().strip()
    load = _load(ticker, valuation_date, m.option_data)
    chain = (
        _iv_chain(ticker, load, m.spot)
        if m.option_data == "chain_iv"
        else _chain(ticker, load)
    )
    return chain.quotes, m.spot, rate, m.dividend, m.valuation_date


def read_store(fn, *args):
    """Call a store reader; an unreachable database is MarketDataUnavailable."""
    try:
        return fn(*args)
    except db.StoreUnavailable as exc:
        raise MarketDataUnavailable(
            f"the market database is unavailable ({exc}); no live fallback is used"
        ) from exc


def local_vol_market(ticker: str, rate: float, valuation_date: date) -> LocalVolMarket:
    """Stored chain + close + dividend yield -> calibrated Dupire grid."""
    with (
        phase("market"),
        tracer.start_as_current_span(
            "market_snapshot.load",
            attributes={"qm.valuation_date": str(valuation_date)},
        ),
    ):
        return _local_vol_market(ticker, rate, valuation_date)


def _local_vol_market(ticker: str, rate: float, valuation_date: date) -> LocalVolMarket:
    ticker = ticker.upper().strip()
    data = option_data()

    load = _load(ticker, valuation_date, data)
    snap = load.snapshot_date
    gap = (valuation_date - snap).days
    if gap > MAX_SNAPSHOT_GAP_DAYS:
        source = "options-chain-iv" if data == "chain_iv" else "options-chain-snapshot"
        raise MarketDataUnavailable(
            f"the latest stored option-chain snapshot for '{ticker}' is "
            f"{snap.isoformat()}, {gap} days before the valuation date "
            f"{valuation_date.isoformat()} (at most {MAX_SNAPSHOT_GAP_DAYS} "
            f"allowed). Refresh it with `ingest run {source}`, "
            f"or value on the stored market date: valuation_date={snap.isoformat()}."
        )

    spot_row = read_store(db.price_on_or_before, ticker, snap)
    if spot_row is None or (snap - spot_row[0]).days > MAX_SPOT_GAP_DAYS:
        have = f"latest is {spot_row[0].isoformat()}" if spot_row else "none stored"
        raise MarketDataUnavailable(
            f"no close for '{ticker}' within {MAX_SPOT_GAP_DAYS} days of the "
            f"snapshot {snap.isoformat()} in prices.sp500_daily ({have}). "
            "Refresh it with `ingest run sp500-prices`."
        )
    spot_date, spot = spot_row

    div_row = read_store(db.dividend_yield_on_or_before, ticker, snap)
    if div_row is None or (snap - div_row[0]).days > MAX_DIVIDEND_GAP_DAYS:
        raise MarketDataUnavailable(
            f"no recent dividend yield for '{ticker}' in prices.dividend_yields "
            "(a non-payer is stored as 0.0, so a missing row means it was never "
            "ingested: dividend-yields covers the options-chain default universe "
            "only). No live fallback is used."
        )
    dividend = div_row[1]

    chain = (
        _iv_chain(ticker, load, spot) if data == "chain_iv" else _chain(ticker, load)
    )
    if not chain.quotes:
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
        f"spot:{ticker}",
        "db:prices.sp500_daily",
        spot_date.isoformat(),
        spot_status,
        spot,
    )
    record_market_input(
        f"dividend:{ticker}",
        "db:prices.dividend_yields",
        div_row[0].isoformat(),
        MarketInputStatus.OBSERVED,
        dividend,
    )
    # The chain's fingerprint is the registry's checksum when there is one:
    # a replay compares it with the registry, so a revised chain is detected
    # without reading its rows.
    record_market_input(
        f"option_chain:{ticker}",
        chain.source,
        snap.isoformat(),
        MarketInputStatus.OBSERVED,
        chain.fingerprint,
    )

    # The calibration is a function of what was just read: cached on a
    # digest of it (the chain enters by its fingerprint, so a revised chain
    # is a new key, not a stale hit).
    surface_key = hashlib.sha256(
        json.dumps(
            [ticker, snap.isoformat(), spot, rate, dividend, chain.fingerprint, data]
        ).encode()
    ).hexdigest()
    cleaning = qm.CleaningParams()
    cleaning.ingestion_cleaned = data == "chain_iv"
    with phase("calibration"):
        result = _surfaces.get(
            surface_key,
            lambda: qm.calibrate_vol_surface(
                chain.quotes,
                spot,
                rate,
                dividend,
                K_MIN,
                K_MAX,
                N_STRIKES,
                N_MATURITIES,
                cleaning_params=cleaning,
            ),
        )

    warnings: List[Dict[str, str]] = [dict(w) for w in chain.warnings]
    if data == "chain_iv":
        warnings.append(
            {
                "code": "surface_on_market_forwards",
                "severity": "info",
                "message": (
                    "The implied-vol surface sits on the market's forwards "
                    "(options.chain_iv); the pricer still drifts at the "
                    "request's rate and the stored dividend yield (WP 31 L6)."
                ),
            }
        )
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
        K_grid=list(result["K_grid"]),
        T_grid=list(result["T_grid"]),
        sigma_loc_flat=list(result["sigma_loc_flat"]),
        warnings=warnings,
        svi_slices=[dict(sl) for sl in result.get("slices", [])],
        rate=rate,
        k_min=float(result.get("k_min", K_MIN)),
        k_max=float(result.get("k_max", K_MAX)),
        n_strikes=N_STRIKES,
        n_maturities=N_MATURITIES,
        surface_model=str(result.get("surface_model", "svi")),
        essvi_forward_smoothing=float(result.get("essvi_forward_smoothing", 0.1)),
        essvi_min_forward_vol=float(result.get("essvi_min_forward_vol", 0.05)),
        option_data=data,
    )


def _load(ticker: str, valuation_date: date, data: str):
    """The registry row of the chain `data` names, latest on or before the
    valuation date."""
    if data == "chain_iv":
        load = read_store(db.options_chain_iv_load_on_or_before, ticker, valuation_date)
        if load is None:
            raise MarketDataUnavailable(
                f"no computed option chain (options.chain_iv) for '{ticker}' on "
                f"or before {valuation_date.isoformat()}. data-ingest's "
                "options-chain-iv source computes it from the stored snapshots "
                f"({OPTION_DATA_ENV}=chain_iv)."
            )
        return load
    load = read_store(db.options_chain_load_on_or_before, ticker, valuation_date)
    if load is None:
        raise MarketDataUnavailable(
            f"no option-chain snapshot for '{ticker}' on or before "
            f"{valuation_date.isoformat()} in the database. data-ingest's "
            "options-chain-snapshot source tracks a fixed universe "
            "(OPTIONS_CHAIN_TICKERS); a ticker outside it has no stored surface."
        )
    return load
