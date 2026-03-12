"""
Stage 1 — Option chain fetcher.

Responsibility: hit yfinance, collect every available expiration, return a
homogeneous list of OptionQuote dataclass objects.  No cleaning or filtering
happens here; this module only knows how to *get* the data.

Public API
----------
fetch_option_chain(ticker) -> list[OptionQuote]
"""
from __future__ import annotations

import logging
import math
from dataclasses import dataclass
from datetime import date, datetime
from typing import List, Optional

import numpy as np
from scipy.optimize import brentq
from scipy.stats import norm
import yfinance as yf

logger = logging.getLogger(__name__)


@dataclass(frozen=True, slots=True)
class OptionQuote:
    """A single option market quote — one row from the chain."""

    ticker: str
    option_type: str        # "call" or "put"
    expiry: date
    ttm: float              # time-to-maturity in years (>0)
    strike: float
    bid: float
    ask: float
    mid: float              # (bid+ask)/2; falls back to last if both are 0
    last: float
    open_interest: int
    volume: int
    implied_volatility: Optional[float]  # as reported by yfinance (may need re-inversion)


def _ttm(expiry: date) -> float:
    """Calendar-day TTM as a fraction of 365."""
    return max((expiry - date.today()).days, 0) / 365.0


def _mid(bid: float, ask: float, last: float) -> float:
    if bid > 0 and ask > 0:
        return (bid + ask) / 2.0
    return last  # fall back to last trade


_MIN_PLAUSIBLE_IV = 0.01  # 1 % — anything below this for equity options is suspect


def _bs_price(S: float, K: float, T: float, r: float, sigma: float, is_call: bool) -> float:
    """Black-Scholes European option price (no dividends)."""
    d1 = (math.log(S / K) + (r + 0.5 * sigma ** 2) * T) / (sigma * math.sqrt(T))
    d2 = d1 - sigma * math.sqrt(T)
    if is_call:
        return S * norm.cdf(d1) - K * math.exp(-r * T) * norm.cdf(d2)
    return K * math.exp(-r * T) * norm.cdf(-d2) - S * norm.cdf(-d1)


def _invert_bs_iv(
    price: float, spot: float, strike: float, ttm: float,
    rate: float, is_call: bool,
) -> Optional[float]:
    """
    Recover implied volatility from an option price via Brent's method.
    Returns None if the price is outside no-arbitrage bounds or inversion fails.
    """
    intrinsic = max(spot - strike, 0.0) if is_call else max(strike - spot, 0.0)
    df = math.exp(-rate * ttm)
    upper_bound = spot if is_call else strike * df
    if price <= intrinsic * df or price >= upper_bound:
        return None
    try:
        iv = brentq(
            lambda sigma: _bs_price(spot, strike, ttm, rate, sigma, is_call) - price,
            1e-4, 5.0,
            xtol=1e-6,
            maxiter=100,
        )
        return iv if 1e-4 < iv < 5.0 else None
    except (ValueError, RuntimeError):
        return None


def _parse_chain_df(
    df, option_type: str, expiry: date, ticker: str, spot: float, rate: float,
) -> List[OptionQuote]:
    """Convert one options DataFrame (calls or puts) into OptionQuote objects."""
    ttm = _ttm(expiry)
    if ttm <= 0:
        return []

    is_call = option_type == "call"
    quotes: List[OptionQuote] = []
    for row in df.itertuples(index=False):
        try:
            strike = float(getattr(row, "strike", 0))
            if strike <= 0:
                continue

            bid   = float(getattr(row, "bid",  0) or 0)
            ask   = float(getattr(row, "ask",  0) or 0)
            last  = float(getattr(row, "lastPrice", 0) or 0)
            oi    = int(getattr(row, "openInterest",  0) or 0)
            vol   = int(getattr(row, "volume",        0) or 0)
            iv_raw = getattr(row, "impliedVolatility", None)
            iv = float(iv_raw) if (iv_raw is not None and iv_raw == iv_raw and float(iv_raw) > 0) else None

            # When yfinance IV is implausibly low, recompute from price
            mid = _mid(bid, ask, last)
            if (iv is None or iv < _MIN_PLAUSIBLE_IV) and mid > 0 and spot > 0:
                iv = _invert_bs_iv(mid, spot, strike, ttm, rate, is_call)

            quotes.append(OptionQuote(
                ticker=ticker,
                option_type=option_type,
                expiry=expiry,
                ttm=ttm,
                strike=strike,
                bid=bid,
                ask=ask,
                mid=mid,
                last=last,
                open_interest=oi,
                volume=vol,
                implied_volatility=iv,
            ))
        except (ValueError, TypeError, AttributeError):
            continue  # skip malformed row silently

    return quotes


def fetch_option_chain(ticker: str, spot: float = 0.0, rate: float = 0.05) -> List[OptionQuote]:
    """
    Fetch the full option chain for *ticker* (all available expirations,
    both calls and puts) and return it as a flat list of OptionQuote.

    Parameters
    ----------
    ticker : str
        Stock ticker symbol, e.g. ``"AAPL"``.
    spot : float
        Current spot price (used for IV recomputation when yfinance IV is bad).
    rate : float
        Risk-free rate (used for IV recomputation).

    Returns
    -------
    list[OptionQuote]
        All quotes across all expirations.  May be empty if yfinance has no
        options data for the ticker.

    Raises
    ------
    RuntimeError
        If yfinance itself raises an unrecoverable error.
    """
    tick = yf.Ticker(ticker)

    try:
        expirations = tick.options
    except Exception as exc:
        raise RuntimeError(f"Failed to fetch expirations for '{ticker}': {exc}") from exc

    if not expirations:
        logger.warning("fetcher: no options available for %s", ticker)
        return []

    quotes: List[OptionQuote] = []

    for exp_str in expirations:
        try:
            expiry = datetime.strptime(exp_str, "%Y-%m-%d").date()
        except ValueError:
            logger.debug("fetcher: could not parse expiry '%s' — skipping", exp_str)
            continue

        if _ttm(expiry) <= 0:
            continue

        try:
            chain = tick.option_chain(exp_str)
        except Exception as exc:
            logger.warning("fetcher: failed to fetch chain for %s %s: %s", ticker, exp_str, exc)
            continue

        if chain.calls is not None and not chain.calls.empty:
            quotes.extend(_parse_chain_df(chain.calls, "call", expiry, ticker, spot, rate))

        if chain.puts is not None and not chain.puts.empty:
            quotes.extend(_parse_chain_df(chain.puts, "put", expiry, ticker, spot, rate))

    logger.info(
        "fetcher: fetched %d quotes for %s across %d expirations",
        len(quotes), ticker, len(expirations),
    )
    return quotes
