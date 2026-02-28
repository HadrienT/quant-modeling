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
from dataclasses import dataclass
from datetime import date, datetime
from typing import List, Optional

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


def _parse_chain_df(df, option_type: str, expiry: date, ticker: str) -> List[OptionQuote]:
    """Convert one options DataFrame (calls or puts) into OptionQuote objects."""
    ttm = _ttm(expiry)
    if ttm <= 0:
        return []

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

            quotes.append(OptionQuote(
                ticker=ticker,
                option_type=option_type,
                expiry=expiry,
                ttm=ttm,
                strike=strike,
                bid=bid,
                ask=ask,
                mid=_mid(bid, ask, last),
                last=last,
                open_interest=oi,
                volume=vol,
                implied_volatility=iv,
            ))
        except (ValueError, TypeError, AttributeError):
            continue  # skip malformed row silently

    return quotes


def fetch_option_chain(ticker: str) -> List[OptionQuote]:
    """
    Fetch the full option chain for *ticker* (all available expirations,
    both calls and puts) and return it as a flat list of OptionQuote.

    Parameters
    ----------
    ticker : str
        Stock ticker symbol, e.g. ``"AAPL"``.

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
            quotes.extend(_parse_chain_df(chain.calls, "call", expiry, ticker))

        if chain.puts is not None and not chain.puts.empty:
            quotes.extend(_parse_chain_df(chain.puts, "put", expiry, ticker))

    logger.info(
        "fetcher: fetched %d quotes for %s across %d expirations",
        len(quotes), ticker, len(expirations),
    )
    return quotes
