"""Market inputs of a multi-underlying script (spot(0), spot(1), ...), from
the database data-ingest fills -- never a live source.

Per asset: the close on or before the valuation date, the dividend yield, and
one flat volatility. The volatility is the at-the-money implied vol of the
asset's own stored smile at the script's last event (vol_smile.py) when an
option chain is stored within market_snapshot.MAX_SNAPSHOT_GAP_DAYS;
otherwise the 63-business-day realised vol, labelled `proxied`. The
correlation matrix is that of log returns between consecutive closes over
the year to the valuation date, on the dates every asset has a close (gaps
in the stored history are reported): estimated on common
dates, it is positive semi-definite by construction.

Every input is recorded for the valuation record (valuation.py), with its
date, source and status.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from datetime import date, timedelta
from typing import Dict, List

import numpy as np

from . import db, market_snapshot, vol_smile
from .audit.payloads import MarketInputStatus
from .market_snapshot import MarketDataUnavailable
from .valuation import record_market_input

#: Correlation window (calendar days) and fewest common daily returns.
CORR_DAYS = 365
CORR_MIN_RETURNS = 60
#: A gap between common closes longer than this (a long weekend plus a
#: holiday) means missing data, reported with the correlation.
MAX_GAP_DAYS = 5


@dataclass
class AssetInput:
    ticker: str
    spot: float
    spot_date: date
    dividend: float
    vol: float
    #: Where the vol comes from, in words (shown on the page).
    vol_source: str
    vol_status: MarketInputStatus


@dataclass
class MultiAssetMarket:
    assets: List[AssetInput]
    correlation: List[List[float]]
    correlation_source: str
    warnings: List[Dict[str, str]] = field(default_factory=list)


def _store(fn, *args):
    try:
        return fn(*args)
    except db.StoreUnavailable as exc:
        raise MarketDataUnavailable(
            f"the market database is unavailable ({exc}); no live fallback is used"
        ) from exc


def _asset(
    ticker: str, rate: float, valuation_date: date, horizon: float, warnings: list
) -> AssetInput:
    spot_row = _store(db.price_on_or_before, ticker, valuation_date)
    gap = (valuation_date - spot_row[0]).days if spot_row else None
    if spot_row is None or gap > market_snapshot.MAX_SPOT_GAP_DAYS:
        have = f"latest is {spot_row[0].isoformat()}" if spot_row else "none stored"
        raise MarketDataUnavailable(
            f"no close for '{ticker}' within {market_snapshot.MAX_SPOT_GAP_DAYS} "
            f"days of {valuation_date.isoformat()} in prices.sp500_daily ({have})."
        )
    spot_date, spot = spot_row
    stale = gap > market_snapshot.SPOT_GAP_WARN_DAYS
    record_market_input(
        f"spot:{ticker}",
        "db:prices.sp500_daily",
        spot_date.isoformat(),
        MarketInputStatus.STALE if stale else MarketInputStatus.OBSERVED,
        spot,
    )

    div_row = _store(db.dividend_yield_on_or_before, ticker, valuation_date)
    if (
        div_row is None
        or (valuation_date - div_row[0]).days > market_snapshot.MAX_DIVIDEND_GAP_DAYS
    ):
        raise MarketDataUnavailable(
            f"no recent dividend yield for '{ticker}' in prices.dividend_yields "
            "(a non-payer is stored as 0.0, so a missing row means it was never "
            "ingested). No live fallback is used."
        )
    record_market_input(
        f"dividend:{ticker}",
        "db:prices.dividend_yields",
        div_row[0].isoformat(),
        MarketInputStatus.OBSERVED,
        div_row[1],
    )

    smile = vol_smile.smile_for(ticker, valuation_date, lambda _snap: rate)
    if smile is not None:
        vol = smile.implied_vol(float(spot), max(horizon, 1e-4))
        source = f"at-the-money implied vol, SVI smile of {smile.snapshot.isoformat()}"
        status, as_of = MarketInputStatus.OBSERVED, smile.snapshot
        table = "db:options.chain_snapshot"
    else:
        from .portfolio_valuation import _OnOrBefore, realised_vol_of

        since = valuation_date - timedelta(days=CORR_DAYS)
        hist = _OnOrBefore(_store(db.price_history, ticker, since))
        got = realised_vol_of(hist, valuation_date)
        if got is None:
            raise MarketDataUnavailable(
                f"'{ticker}' has neither a stored option chain nor enough closes "
                "for a realised vol"
            )
        as_of, vol = got
        source = "63-day realised vol (no option chain stored)"
        status = MarketInputStatus.PROXIED
        table = "db:prices.sp500_daily"
        warnings.append(
            {
                "code": "vol_proxied",
                "severity": "warning",
                "message": f"{ticker} has no stored option chain: its volatility "
                f"is the 63-day realised vol ({vol:.1%}), not an implied vol.",
            }
        )
    record_market_input(f"vol:{ticker}", table, as_of.isoformat(), status, vol)
    return AssetInput(
        ticker=ticker,
        spot=float(spot),
        spot_date=spot_date,
        dividend=float(div_row[1]),
        vol=float(vol),
        vol_source=source,
        vol_status=status,
    )


def _correlation(tickers: List[str], valuation_date: date, warnings: list) -> tuple:
    since = valuation_date - timedelta(days=CORR_DAYS)
    wide = _store(db.prices_wide, tickers, since)
    missing = [t for t in tickers if t not in getattr(wide, "columns", [])]
    if missing:
        raise MarketDataUnavailable(
            f"no stored closes for {', '.join(missing)} in the year to "
            f"{valuation_date.isoformat()}: no correlation can be estimated"
        )
    wide = wide[[t for t in tickers]]
    wide = wide[wide.index.date <= valuation_date].dropna()
    returns = np.diff(np.log(wide.to_numpy(dtype=float)), axis=0)
    if len(returns) < CORR_MIN_RETURNS:
        raise MarketDataUnavailable(
            f"only {len(returns)} common daily returns for {', '.join(tickers)} "
            f"in the year to {valuation_date.isoformat()} (at least "
            f"{CORR_MIN_RETURNS} needed for a correlation)"
        )
    corr = np.corrcoef(returns, rowvar=False)
    corr = (corr + corr.T) / 2.0
    np.fill_diagonal(corr, 1.0)
    dates = [d.date() for d in wide.index]
    first, last = dates[0], dates[-1]
    source = (
        f"historical, log returns between {len(dates)} common closes from "
        f"{first.isoformat()} to {last.isoformat()}"
    )
    gaps = [(a, b) for a, b in zip(dates, dates[1:]) if (b - a).days > MAX_GAP_DAYS]
    if gaps:
        a, b = max(gaps, key=lambda g: (g[1] - g[0]).days)
        warnings.append(
            {
                "code": "correlation_sparse_history",
                "severity": "warning",
                "message": f"The stored closes of {', '.join(tickers)} have "
                f"{len(gaps)} gap(s) longer than {MAX_GAP_DAYS} days in the "
                f"year (the longest from {a.isoformat()} to {b.isoformat()}): "
                f"the correlation rests on {len(returns)} returns, some spanning "
                "several weeks, instead of about 250 daily ones.",
            }
        )
    record_market_input(
        f"correlation:{'/'.join(tickers)}",
        "db:prices.sp500_daily",
        last.isoformat(),
        MarketInputStatus.OBSERVED,
        [[round(float(x), 12) for x in row] for row in corr],
    )
    return corr.tolist(), source


def multi_asset_market(
    tickers: List[str], rate: float, valuation_date: date, horizon: float
) -> MultiAssetMarket:
    """Spots, dividends, flat vols and the correlation of `tickers` on
    `valuation_date`. `horizon` (years) is the script's last event: the
    maturity the implied vols are read at."""
    tickers = [t.upper().strip() for t in tickers]
    if len(set(tickers)) != len(tickers):
        raise ValueError("each underlying must be a different ticker")
    warnings: List[Dict[str, str]] = []
    assets = [_asset(t, rate, valuation_date, horizon, warnings) for t in tickers]
    corr, source = _correlation(tickers, valuation_date, warnings)
    return MultiAssetMarket(assets, corr, source, warnings)


__all__ = ["AssetInput", "MultiAssetMarket", "multi_asset_market"]
