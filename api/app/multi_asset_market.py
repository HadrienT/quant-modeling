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

Quantos and composites (issue #86). An underlying may also be an exchange
rate (`FxRate`: the price of one unit of a currency in the payment
currency, ECB fixings), and the payment currency may differ from an asset's
listing currency. Everything is then simulated under the payment currency's
measure (Reiner 1992; Hull, "Quantos"): an exchange rate drifts at
r_d - r_f, and an asset listed in currency f at r_f - q - rho sigma_S
sigma_X, X its currency's rate in the payment currency. A script paying
f(spot(i)) is then a quanto, one paying f(spot(i) * spot(j)) with spot(j)
that rate a composite -- the model is the same, the payoff says which.
r_f is the zero rate of f's stored curve at the script's horizon; sigma_X
the 63-day realised vol of the fixings (no FX options are stored); rho the
historical correlation. Series from different markets are not observed at
the same instant, and correlating their daily returns biases the estimate
towards zero (the Epps effect, see fx.py): a matrix mixing currencies uses
weekly returns over three years.

Every input is recorded for the valuation record (valuation.py), with its
date, source and status.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from datetime import date, timedelta
from typing import Dict, List, NamedTuple, Optional, Sequence, Tuple, Union

import numpy as np
import pandas as pd

from . import db, fx, market_snapshot, vol_smile
from .audit.payloads import MarketInputStatus
from .market_snapshot import MarketDataUnavailable
from .valuation import record_market_input

#: Correlation window (calendar days) and fewest common daily returns.
CORR_DAYS = 365
CORR_MIN_RETURNS = 60
#: A gap between common closes longer than this (a long weekend plus a
#: holiday) means missing data, reported with the correlation.
MAX_GAP_DAYS = 5
#: Window of the weekly correlation of series from different markets: about
#: 156 weekly returns, as many as a year of daily ones would need.
MIXED_CORR_DAYS = 3 * 365


class FxRate(NamedTuple):
    """An exchange rate as underlying: one unit of `currency` in the payment
    currency."""

    currency: str


@dataclass
class AssetInput:
    ticker: Optional[str]
    spot: float
    spot_date: date
    dividend: float
    vol: float
    #: Where the vol comes from, in words (shown on the page).
    vol_source: str
    vol_status: MarketInputStatus
    #: The currency the spot is in (the payment currency for an FX rate).
    currency: str = ""
    #: An exchange-rate underlying: its foreign currency.
    fx: Optional[str] = None
    #: Added to the dividend yield in the simulation: r_f for an exchange
    #: rate, r_d - r_f + rho sigma_S sigma_X for an asset in a foreign
    #: currency (the quanto drift), 0 otherwise.
    drift_adjustment: float = 0.0
    drift_source: str = ""


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
            f"days of {valuation_date.isoformat()} in the stored closes ({have})."
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


def _foreign_rate(ccy: str, valuation_date: date, horizon: float) -> float:
    """r_f: the zero rate of `ccy`'s stored curve at the script's horizon."""
    from .portfolio_valuation import MarketData

    got = MarketData(valuation_date).zero_rate(
        ccy, valuation_date, max(horizon, 1.0 / 12.0)
    )
    if got is None:
        raise MarketDataUnavailable(
            f"no stored {ccy} discount curve on or before "
            f"{valuation_date.isoformat()}: the {ccy} rate of the quanto drift "
            "cannot be read (the GBP and CHF curves are not derivable from the "
            "stored series)"
        )
    record_market_input(
        f"rate:{ccy}",
        "db:rates",
        got[0].isoformat(),
        MarketInputStatus.OBSERVED,
        got[1],
    )
    return float(got[1])


def _fx_series(ccy: str, pay: str, since: date, valuation_date: date) -> pd.Series:
    try:
        s = fx.pair_history(ccy, pay, since)
    except fx.FxUnavailable as exc:
        raise MarketDataUnavailable(str(exc)) from exc
    s.index = pd.to_datetime(s.index)
    return s[s.index.date <= valuation_date].dropna()


def _fx_vol(ccy: str, pay: str, valuation_date: date) -> Tuple[date, float, pd.Series]:
    """63-day realised vol of the fixings of `ccy` in `pay`, and the series."""
    from .portfolio_valuation import _OnOrBefore, realised_vol_of

    s = _fx_series(ccy, pay, valuation_date - timedelta(days=CORR_DAYS), valuation_date)
    got = realised_vol_of(
        _OnOrBefore([(d.date(), float(v)) for d, v in s.items()]), valuation_date
    )
    if got is None:
        raise MarketDataUnavailable(
            f"too few stored {ccy}/{pay} fixings before {valuation_date.isoformat()} "
            "for a realised vol"
        )
    record_market_input(
        f"vol:{ccy}{pay}",
        "db:fx.ecb_reference_rates",
        got[0].isoformat(),
        MarketInputStatus.PROXIED,
        got[1],
    )
    return got[0], got[1], s


def _fx_asset(
    ccy: str, pay: str, valuation_date: date, horizon: float, warnings: list
) -> AssetInput:
    as_of, vol, s = _fx_vol(ccy, pay, valuation_date)
    spot_date, spot = s.index[-1].date(), float(s.iloc[-1])
    if (valuation_date - spot_date).days > market_snapshot.MAX_SPOT_GAP_DAYS:
        raise MarketDataUnavailable(
            f"the latest stored {ccy}/{pay} fixing is {spot_date.isoformat()}, "
            f"more than {market_snapshot.MAX_SPOT_GAP_DAYS} days before "
            f"{valuation_date.isoformat()}"
        )
    record_market_input(
        f"spot:{ccy}{pay}",
        "db:fx.ecb_reference_rates",
        spot_date.isoformat(),
        MarketInputStatus.OBSERVED,
        spot,
    )
    r_f = _foreign_rate(ccy, valuation_date, horizon)
    warnings.append(
        {
            "code": "fx_vol_proxied",
            "severity": "warning",
            "message": f"The {ccy}/{pay} volatility is the 63-day realised vol of "
            f"the ECB fixings ({vol:.1%}): no FX options are stored.",
        }
    )
    return AssetInput(
        ticker=None,
        spot=spot,
        spot_date=spot_date,
        dividend=0.0,
        vol=vol,
        vol_source="63-day realised vol of the ECB fixings (no FX options stored)",
        vol_status=MarketInputStatus.PROXIED,
        currency=pay,
        fx=ccy,
        drift_adjustment=r_f,
        drift_source=f"{ccy} zero rate at {horizon:.2f}y: a rate drifts at r_d - r_f",
    )


def _mixed_correlation(
    names: List[str], series: List[pd.Series], valuation_date: date, warnings: list
) -> List[List[float]]:
    """Weekly log returns on common weeks over MIXED_CORR_DAYS: the series
    are closes on different exchanges and ECB fixings, not simultaneous."""
    wide = pd.concat(series, axis=1, join="inner").dropna()
    wide.columns = names
    weekly = wide.resample("W-FRI").last().dropna()
    returns = np.diff(np.log(weekly.to_numpy(dtype=float)), axis=0)
    if len(returns) < CORR_MIN_RETURNS:
        raise MarketDataUnavailable(
            f"only {len(returns)} common weekly returns for {', '.join(names)} "
            f"in the {MIXED_CORR_DAYS // 365} years to {valuation_date.isoformat()} "
            f"(at least {CORR_MIN_RETURNS} needed for a correlation)"
        )
    corr = np.corrcoef(returns, rowvar=False)
    corr = (corr + corr.T) / 2.0
    np.fill_diagonal(corr, 1.0)
    record_market_input(
        f"correlation:{'/'.join(names)}",
        "db:prices.sp500_daily+fx.ecb_reference_rates",
        weekly.index[-1].date().isoformat(),
        MarketInputStatus.OBSERVED,
        [[round(float(x), 12) for x in row] for row in corr],
    )
    return corr.tolist()


Underlying = Union[str, FxRate]


def multi_asset_market(
    underlyings: Sequence[Underlying],
    rate: float,
    valuation_date: date,
    horizon: float,
    currency: Optional[str] = None,
) -> MultiAssetMarket:
    """Spots, dividends, flat vols and the correlation of `underlyings`
    (tickers, or FxRate) on `valuation_date`, under the measure of the
    payment `currency` (default: the first ticker's listing currency), whose
    rate is `rate`. `horizon` (years) is the script's last event: the
    maturity the implied vols and foreign rates are read at."""
    tickers = [u.upper().strip() for u in underlyings if isinstance(u, str)]
    rates_fx = [u.currency.upper() for u in underlyings if isinstance(u, FxRate)]
    if len(set(tickers)) != len(tickers) or len(set(rates_fx)) != len(rates_fx):
        raise ValueError("each underlying must be a different ticker or currency")
    ccy = {t: _store(db.ticker_currency, t) or "USD" for t in tickers}
    if currency is None and not tickers:
        raise ValueError("give the payment currency of a script on exchange rates only")
    pay = (currency or ccy[tickers[0]]).upper()
    if pay in rates_fx:
        raise ValueError(f"{pay} is the payment currency: its rate in itself is 1")
    warnings: List[Dict[str, str]] = []
    r_f = {
        c: _foreign_rate(c, valuation_date, horizon)
        for c in set(ccy.values())
        if c != pay
    }

    assets: List[AssetInput] = []
    for u in underlyings:
        if isinstance(u, FxRate):
            assets.append(
                _fx_asset(u.currency.upper(), pay, valuation_date, horizon, warnings)
            )
            continue
        t = u.upper().strip()
        a = _asset(t, r_f.get(ccy[t], rate), valuation_date, horizon, warnings)
        a.currency = ccy[t]
        assets.append(a)

    if not r_f and not rates_fx:
        if len(tickers) == 1:
            return MultiAssetMarket(assets, [[1.0]], "one underlying", warnings)
        corr, source = _correlation(tickers, valuation_date, warnings)
        return MultiAssetMarket(assets, corr, source, warnings)

    # Several currencies: every underlying's series, plus the rate of each
    # foreign listing currency that is not an underlying itself (its
    # correlation with the asset makes the quanto drift).
    since = valuation_date - timedelta(days=MIXED_CORR_DAYS)
    names, series = [], []
    for a in assets:
        if a.fx is not None:
            names.append(f"{a.fx}{pay}")
            series.append(_fx_series(a.fx, pay, since, valuation_date))
        else:
            rows = _store(db.price_history, a.ticker, since)
            names.append(a.ticker)
            series.append(
                pd.Series(
                    [float(v) for _, v in rows],
                    index=pd.to_datetime([d for d, _ in rows]),
                )
            )
    hidden = sorted(c for c in r_f if c not in rates_fx)
    for c in hidden:
        names.append(f"{c}{pay}")
        series.append(_fx_series(c, pay, since, valuation_date))
    full = _mixed_correlation(names, series, valuation_date, warnings)

    fx_vol = {a.fx: a.vol for a in assets if a.fx is not None}
    for c in hidden:
        fx_vol[c] = _fx_vol(c, pay, valuation_date)[1]
    adjusted = []
    for i, a in enumerate(assets):
        if a.fx is not None or a.currency == pay:
            continue
        rho = full[i][names.index(f"{a.currency}{pay}")]
        a.drift_adjustment = rate - r_f[a.currency] + rho * a.vol * fx_vol[a.currency]
        a.drift_source = (
            f"quanto drift: r_{pay} - r_{a.currency} + rho sigma_S sigma_X with "
            f"r_{a.currency} = {r_f[a.currency]:.2%}, rho = {rho:.2f} to "
            f"{a.currency}/{pay}, sigma_X = {fx_vol[a.currency]:.1%}"
        )
        adjusted.append(f"{a.ticker} ({a.currency}) {a.drift_adjustment:+.2%}")
    if adjusted:
        warnings.append(
            {
                "code": "quanto_drift",
                "severity": "info",
                "message": f"Paid in {pay}: the assets listed in another currency "
                f"are simulated under the {pay} measure, their drift lowered by "
                f"r_d - r_f + rho sigma_S sigma_X: {'; '.join(adjusted)}.",
            }
        )
    n = len(assets)
    corr = [row[:n] for row in full[:n]]
    source = (
        f"historical, weekly log returns over {MIXED_CORR_DAYS // 365} years "
        "(closes on different exchanges and ECB fixings are not simultaneous: "
        "daily returns would understate the correlation)"
    )
    return MultiAssetMarket(assets, corr, source, warnings)


__all__ = ["AssetInput", "FxRate", "MultiAssetMarket", "multi_asset_market"]
