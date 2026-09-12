"""
Vol surface pipeline: option chain -> SVI per maturity -> Dupire local vol.

Replaces api/app/local_vol/{fetcher,cleaner,iv_surface,dupire,cpp_bridge}.py
and routers/market_iv_surface.py's independent yfinance/griddata path --
those had drifted into two unrelated implementations of the same problem.
There is one fetch, and one C++ pipeline (market/vol_surface_pipeline.hpp,
bound as quantmodeling.calibrate_vol_surface) for everything past it:
cleaning, SVI calibration per maturity, and the Dupire local-vol grid.

Fetching stays in Python and has two sources, in order of preference:
  1. data-ingest's Postgres (options.chain_snapshot) -- historical, free of
     yfinance rate limits, but only for the small fixed ticker universe
     options-chain-snapshot tracks (see ~/data-ingest's README).
  2. A live yfinance fetch -- any ticker, today's chain only, same mechanics
     as data-ingest's own source (and as the fetcher.py this replaces).
"""

from __future__ import annotations

import logging
import math
from datetime import date, datetime
from typing import List, Optional, Tuple

import quantmodeling as qm
import yfinance as yf

from . import db

logger = logging.getLogger(__name__)

MIN_CLEAN_QUOTES = 16  # below this, a fit is not meaningfully constrained


class NoOptionChainAvailable(RuntimeError):
    """Neither a stored snapshot nor a live fetch produced any quotes."""


def _ttm(expiry: date, as_of: date) -> float:
    """Calendar-day TTM as a fraction of 365, floored at 0 for an expired quote."""
    return max((expiry - as_of).days, 0) / 365.0


def _quote(
    strike: float,
    ttm: float,
    is_call: bool,
    bid: float,
    ask: float,
    last: float,
    volume: int,
    open_interest: int,
    implied_vol: Optional[float],
) -> "qm.RawOptionQuote":
    q = qm.RawOptionQuote()
    q.strike = strike
    q.ttm = ttm
    q.is_call = is_call
    q.bid = bid
    q.ask = ask
    q.last = last
    q.volume = volume
    q.open_interest = open_interest
    has_iv = (
        implied_vol is not None and implied_vol == implied_vol and implied_vol > 0
    )  # NaN-safe
    q.implied_vol = float(implied_vol) if has_iv else 0.0
    q.has_iv = has_iv
    return q


def fetch_live_chain(ticker: str) -> List["qm.RawOptionQuote"]:
    """Every expiration, both sides, straight from yfinance -- no cleaning."""
    return [q for q, _expiry in _fetch_live_chain_with_expiry(ticker)]


def _fetch_live_chain_with_expiry(
    ticker: str,
) -> List[Tuple["qm.RawOptionQuote", date]]:
    """Same fetch as fetch_live_chain, but keeps each quote's expiry date
    alongside it -- RawOptionQuote only carries ttm (a float), and caching a
    live fetch into options.chain_snapshot needs the real date, not ttm
    reconstructed by rounding."""
    tick = yf.Ticker(ticker)
    try:
        expirations = tick.options
    except Exception as exc:
        raise RuntimeError(
            f"Failed to fetch expirations for '{ticker}': {exc}"
        ) from exc
    if not expirations:
        return []

    today = date.today()
    pairs: List[Tuple[qm.RawOptionQuote, date]] = []
    for exp_str in expirations:
        try:
            expiry = datetime.strptime(exp_str, "%Y-%m-%d").date()
        except ValueError:
            continue
        ttm = _ttm(expiry, today)
        if ttm <= 0:
            continue

        try:
            chain = tick.option_chain(exp_str)
        except Exception as exc:
            logger.warning(
                "vol_surface: failed to fetch %s %s: %s", ticker, exp_str, exc
            )
            continue

        for side_df, is_call in ((chain.calls, True), (chain.puts, False)):
            if side_df is None or side_df.empty:
                continue
            for row in side_df.itertuples(index=False):
                try:
                    strike = float(getattr(row, "strike", 0) or 0)
                    if strike <= 0:
                        continue
                    iv = getattr(row, "impliedVolatility", None)
                    quote = _quote(
                        strike=strike,
                        ttm=ttm,
                        is_call=is_call,
                        bid=float(getattr(row, "bid", 0) or 0),
                        ask=float(getattr(row, "ask", 0) or 0),
                        last=float(getattr(row, "lastPrice", 0) or 0),
                        volume=int(getattr(row, "volume", 0) or 0),
                        open_interest=int(getattr(row, "openInterest", 0) or 0),
                        implied_vol=float(iv) if iv is not None else None,
                    )
                    pairs.append((quote, expiry))
                except (ValueError, TypeError, AttributeError):
                    continue  # malformed row: skip, matching the old fetcher.py

    logger.info(
        "vol_surface: fetched %d live quotes for %s across %d expirations",
        len(pairs),
        ticker,
        len(expirations),
    )
    return pairs


def fetch_stored_chain(ticker: str) -> List["qm.RawOptionQuote"]:
    """The most recent snapshot data-ingest has for this ticker, if any."""
    try:
        rows = db.options_chain_snapshot(ticker)
    except db.StoreUnavailable as exc:
        logger.warning(
            "vol_surface: options store unavailable (%s), falling back to live fetch",
            exc,
        )
        return []

    quotes: List[qm.RawOptionQuote] = []
    for r in rows:
        ttm = _ttm(r.expiry, r.snapshot_date)
        if ttm <= 0:
            continue
        quotes.append(
            _quote(
                strike=r.strike,
                ttm=ttm,
                is_call=(r.option_type == "call"),
                bid=r.bid or 0.0,
                ask=r.ask or 0.0,
                last=r.last_price or 0.0,
                volume=r.volume,
                open_interest=r.open_interest,
                implied_vol=r.implied_volatility,
            )
        )
    return quotes


def fetch_option_chain(ticker: str) -> List["qm.RawOptionQuote"]:
    """Stored snapshot if data-ingest tracks this ticker, live yfinance
    otherwise -- and a live fetch is cached back into options.chain_snapshot
    (db.cache_option_chain_snapshot) so a second request for the same ticker
    the same day hits Postgres instead of yfinance again. This is what
    extends options-chain-snapshot's small tracked universe on demand: any
    ticker actually requested gets cached for the rest of that day, same
    table and schema a scheduled run would use.
    """
    ticker = ticker.upper().strip()
    stored = fetch_stored_chain(ticker)
    if stored:
        logger.info(
            "vol_surface: using stored snapshot for %s (%d quotes)", ticker, len(stored)
        )
        return stored

    logger.info("vol_surface: no stored snapshot for %s, fetching live", ticker)
    pairs = _fetch_live_chain_with_expiry(ticker)
    _cache_live_chain(ticker, pairs)
    return [q for q, _expiry in pairs]


def _cache_live_chain(
    ticker: str, pairs: List[Tuple["qm.RawOptionQuote", date]]
) -> None:
    if not pairs:
        return
    rows = [
        {
            "expiry": expiry,
            "option_type": "call" if q.is_call else "put",
            "strike": q.strike,
            "bid": q.bid or None,
            "ask": q.ask or None,
            "last_price": q.last or None,
            "volume": q.volume,
            "open_interest": q.open_interest,
            "implied_volatility": q.implied_vol if q.has_iv else None,
        }
        for q, expiry in pairs
    ]
    try:
        written = db.cache_option_chain_snapshot(ticker, date.today(), rows)
        logger.info(
            "vol_surface: cached %d live quotes for %s into options.chain_snapshot",
            written,
            ticker,
        )
    except db.StoreUnavailable as exc:
        logger.warning(
            "vol_surface: could not cache live chain for %s: %s", ticker, exc
        )


def get_spot(ticker: str) -> float:
    """Latest close from prices.sp500_daily (covers this pipeline's default
    ticker universe, including the index ETFs), live yfinance otherwise --
    not cached back: a single cheap call, unlike a full option chain, and
    sp500-prices already refreshes this same table daily for the tickers
    that matter here."""
    ticker = ticker.upper().strip()
    try:
        cached = db.latest_price(ticker)
    except db.StoreUnavailable:
        cached = None
    if cached is not None:
        as_of, price = cached
        logger.info("vol_surface: spot for %s from DB (%s): %.2f", ticker, as_of, price)
        return price

    logger.info("vol_surface: no stored price for %s, fetching live", ticker)
    return _get_spot_live(ticker)


def _get_spot_live(ticker: str) -> float:
    tick = yf.Ticker(ticker)
    try:
        price = tick.fast_info["last_price"]
        if price and price > 0:
            return float(price)
    except Exception:
        pass
    hist = tick.history(period="1d")
    if hist.empty:
        raise NoOptionChainAvailable(f"No price data found for ticker '{ticker}'")
    return float(hist["Close"].iloc[-1])


def get_dividend_yield(ticker: str) -> float:
    """Latest trailing yield from prices.dividend_yields, live yfinance
    otherwise -- same non-caching rationale as get_spot."""
    ticker = ticker.upper().strip()
    try:
        cached = db.latest_dividend_yield(ticker)
    except db.StoreUnavailable:
        cached = None
    if cached is not None:
        as_of, value = cached
        logger.info(
            "vol_surface: dividend yield for %s from DB (%s): %.4f",
            ticker,
            as_of,
            value,
        )
        return value

    logger.info("vol_surface: no stored dividend yield for %s, fetching live", ticker)
    return _get_dividend_yield_live(ticker)


def _get_dividend_yield_live(ticker: str) -> float:
    """Best-effort: trailing dividend yield as a continuous-rate proxy.

    .info's trailingAnnualDividendYield, not fast_info's dividend_yield --
    the current yfinance version pinned here doesn't carry that field on
    fast_info at all, so reading it there silently returns 0.0 for every
    ticker, dividend payers included. Caught the same way in data-ingest's
    dividend-yields source: by actually running it against live data.
    """
    try:
        dy = yf.Ticker(ticker).info.get("trailingAnnualDividendYield")
        return float(dy) if dy else 0.0
    except Exception:
        return 0.0


# ---------------------------------------------------------------------------
# SVI evaluation on the Python side, for the "cleaned" IV-surface display --
# the calibration result carries per-slice (a, b, rho, m, sigma), and this
# is the same linear-in-T interpolation SVISurface does in C++
# (market/svi_surface.hpp), just for display rather than for feeding Dupire.
# ---------------------------------------------------------------------------


def _svi_total_variance(
    k: float, a: float, b: float, rho: float, m: float, sigma: float
) -> float:
    x = k - m
    return a + b * (rho * x + math.sqrt(x * x + sigma * sigma))


def svi_implied_vol_grid(
    slices: List[dict],
    spot: float,
    rate: float,
    dividend: float,
    k_min: float,
    k_max: float,
    n_strikes: int = 60,
    n_maturities: int = 40,
) -> tuple[List[float], List[float], List[List[Optional[float]]]]:
    """A regular (strike, maturity) grid of SVI-fitted implied vol, linearly
    interpolating total variance in T between adjacent calibrated slices --
    the arbitrage-checked replacement for the old bicubic-spline surface.
    """
    ordered = sorted(slices, key=lambda s: s["ttm"])
    t_lo, t_hi = ordered[0]["ttm"], ordered[-1]["ttm"]

    T_grid = [
        t_lo + i * (t_hi - t_lo) / max(n_maturities - 1, 1) for i in range(n_maturities)
    ]
    T_mid = 0.5 * (t_lo + t_hi)
    F_mid = spot * math.exp((rate - dividend) * T_mid)
    K_grid = [
        F_mid * math.exp(k_min + i * (k_max - k_min) / max(n_strikes - 1, 1))
        for i in range(n_strikes)
    ]

    def variance_at(k: float, T: float) -> float:
        T_clamped = min(max(T, t_lo), t_hi)
        i = 0
        while i + 2 < len(ordered) and ordered[i + 1]["ttm"] < T_clamped:
            i += 1
        s0, s1 = ordered[i], ordered[i + 1]
        lam = (T_clamped - s0["ttm"]) / (s1["ttm"] - s0["ttm"])
        w0 = _svi_total_variance(k, s0["a"], s0["b"], s0["rho"], s0["m"], s0["sigma"])
        w1 = _svi_total_variance(k, s1["a"], s1["b"], s1["rho"], s1["m"], s1["sigma"])
        return (1.0 - lam) * w0 + lam * w1

    values: List[List[Optional[float]]] = []
    for T in T_grid:
        F_T = spot * math.exp((rate - dividend) * T)
        row: List[Optional[float]] = []
        for K in K_grid:
            k = math.log(K / F_T)
            w = variance_at(k, T)
            row.append(round(math.sqrt(w / T), 6) if w > 0 else None)
        values.append(row)

    return K_grid, list(T_grid), values
