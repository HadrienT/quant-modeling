"""FX: spot, forwards, realised volatility and correlations — the FX tab, and
the market inputs of the quanto pricer.

Everything is read from data-ingest's Postgres; nothing is fetched live.

- **Spot**: the ECB euro reference rates (`fx.ecb_reference_rates`, one
  fixing a day around 14:10 CET). A pair BASE/QUOTE is the number of QUOTE
  units for one BASE (EUR/USD 1.1367); crosses by division of the two euro
  rates, so every pair shares one fixing time.
- **Forwards** need no model: covered interest parity,
  F(T) = S · DF_base(T) / DF_quote(T), with each currency's discount curve
  from the rates page (rates.currency_discount_curve). A currency without a
  derivable curve (GBP, CHF) has no forward, and the page says why.
- **Volatility and correlation** are historical estimates from the stored
  series, labelled as such: implied FX volatilities are not published free.

Correlations use weekly returns by default: the series are not observed at
the same instant (ECB fixing 14:10 CET, Tokyo closes 06:00 UTC, New York
20:00 UTC), and correlating daily returns of asynchronous series biases the
estimate towards zero (the Epps effect). Every estimate comes with its sample
size and a 95 % confidence interval (Fisher transform).
"""

from __future__ import annotations

import math
from dataclasses import dataclass
from datetime import date, timedelta
from typing import List, Literal, Optional

import numpy as np
import pandas as pd
import quantmodeling as qm

from . import db, rates

CURRENCIES: tuple[str, ...] = ("USD", "EUR", "GBP", "JPY", "CHF")
TRADING_DAYS = 252
WEEKS = 52
FORWARD_TENORS = (
    ("1M", 1 / 12), ("3M", 0.25), ("6M", 0.5), ("1Y", 1.0), ("2Y", 2.0),
    ("3Y", 3.0), ("5Y", 5.0), ("7Y", 7.0), ("10Y", 10.0),
)  # fmt: skip
#: Spot and curves are published on different days; beyond this gap (calendar
#: days) a forward mixes two markets and is flagged.
MAX_DATE_GAP_DAYS = 7
WINDOWS = {"1Y": 365, "3Y": 3 * 365, "5Y": 5 * 365}

Frequency = Literal["weekly", "daily"]


class FxUnavailable(RuntimeError):
    pass


def pair_history(base: str, quote: str, since: Optional[date] = None) -> pd.Series:
    """QUOTE units per BASE, indexed by fixing date."""
    if base == quote:
        raise FxUnavailable("base and quote are the same currency")
    wide = db.ecb_fx_history([base, quote], since)
    if wide.empty:
        raise FxUnavailable(f"no ECB reference rates for {base}/{quote} in the store")
    return (wide[quote] / wide[base]).rename(f"{base}{quote}")


@dataclass
class ForwardPoint:
    label: str
    tenor: float
    forward: float
    points: float  # forward − spot, in quote units


@dataclass
class Forwards:
    points: List[ForwardPoint]
    curve_dates: dict[str, date]


def forwards(base: str, quote: str, spot: float, spot_date: date) -> Forwards:
    """Covered interest parity on the two government discount curves.
    Only tenors inside both curves (no extrapolation) are returned."""
    try:
        curves = [rates.currency_discount_curve(c) for c in (base, quote)]
    except rates.RatesUnavailable as exc:
        raise FxUnavailable(str(exc)) from exc
    lo = max(c.times[0] for c in curves)
    hi = min(c.times[-1] for c in curves)
    tenors = [(label, t) for label, t in FORWARD_TENORS if lo - 1e-9 <= t <= hi + 1e-9]
    if not tenors:
        raise FxUnavailable("the two curves have no maturity in common")
    ts = [t for _, t in tenors]
    df_base = qm.discount_factors(curves[0].times, curves[0].dfs, ts)
    df_quote = qm.discount_factors(curves[1].times, curves[1].dfs, ts)
    points = []
    for (label, t), db_, dq in zip(tenors, df_base, df_quote):
        f = spot * db_ / dq
        points.append(ForwardPoint(label, t, f, f - spot))
    return Forwards(points, {c.currency: c.as_of for c in curves})


def log_returns(series: pd.Series, frequency: Frequency) -> pd.Series:
    s = series.sort_index()
    s.index = pd.to_datetime(s.index)
    if frequency == "weekly":
        # Friday's (or the week's last) observation: one per week.
        s = s.resample("W-FRI").last().dropna()
    return np.log(s).diff().dropna()


def realised_vol(series: pd.Series, frequency: Frequency = "daily") -> Optional[float]:
    r = log_returns(series, frequency)
    if len(r) < 10:
        return None
    return float(
        r.std(ddof=1) * math.sqrt(TRADING_DAYS if frequency == "daily" else WEEKS)
    )


@dataclass
class Correlation:
    correlation: float
    n: int
    ci_low: float
    ci_high: float
    asset_vol: float
    fx_vol: float
    start: date
    end: date
    asset_last: float  # last common observation: the quanto's spot…
    fx_last: float  # …and a natural fixed conversion rate


def correlation(
    asset: pd.Series, fx_rate: pd.Series, frequency: Frequency
) -> Correlation:
    """Pearson correlation of log returns on common dates, with a 95 %
    confidence interval from the Fisher transform: tanh(atanh(ρ) ± 1.96/√(n−3))."""
    a = log_returns(asset, frequency)
    b = log_returns(fx_rate, frequency)
    last = pd.concat([asset, fx_rate], axis=1, join="inner").dropna()
    joined = pd.concat([a, b], axis=1, join="inner").dropna()
    n = len(joined)
    if n < 20:
        raise FxUnavailable(f"only {n} common {frequency} returns: too few to estimate")
    rho = float(joined.iloc[:, 0].corr(joined.iloc[:, 1]))
    half = 1.959964 / math.sqrt(n - 3)
    z = math.atanh(max(min(rho, 0.999999), -0.999999))
    annual = math.sqrt(TRADING_DAYS if frequency == "daily" else WEEKS)
    return Correlation(
        rho,
        n,
        math.tanh(z - half),
        math.tanh(z + half),
        float(joined.iloc[:, 0].std(ddof=1) * annual),
        float(joined.iloc[:, 1].std(ddof=1) * annual),
        joined.index[0].date(),
        joined.index[-1].date(),
        float(last.iloc[-1, 0]),
        float(last.iloc[-1, 1]),
    )


def asset_series(ticker: str, since: date) -> pd.Series:
    rows = db.price_history(ticker, since)
    if not rows:
        raise FxUnavailable(f"no price history for {ticker}")
    return pd.Series([c for _, c in rows], index=[d for d, _ in rows], name=ticker)


def window_start(window: str) -> date:
    return date.today() - timedelta(days=WINDOWS[window])


# ── Methodology (shown on the page) ──────────────────────────────────────────

METHODOLOGY = [
    (
        "Spot",
        [
            "Spot rates are the ECB's euro foreign-exchange reference rates, fixed once a "
            "day around 14:10 CET. A pair BASE/QUOTE is the number of QUOTE units for one "
            "BASE; a pair without the euro is a cross, the ratio of the two euro rates on "
            "the same day, so both legs share one fixing time.",
        ],
    ),
    (
        "Forwards",
        [
            "Forwards follow from covered interest parity, with no model: investing one "
            "unit of BASE at its rate, or converting it and investing QUOTE at its rate, "
            "must be worth the same at maturity, so F(T) = S · DF_base(T) / DF_quote(T). "
            "The discount factors come from each currency's government curve on the Rates "
            "tab (US Treasury, euro-area AAA, JGB). A currency without a derivable curve "
            "(GBP, CHF) has no forward here.",
            "These are theoretical forwards. Market forwards differ by the cross-currency "
            "basis (a few to a few tens of basis points a year since 2008) and are priced "
            "off OIS rather than government curves; neither is published free. Forward "
            "points are F − S, in QUOTE units.",
        ],
    ),
    (
        "Volatility and correlation",
        [
            "Both are historical, from the stored series — not implied: FX implied "
            "volatilities are not published free. Volatility is the standard deviation of "
            "log returns, annualised with 252 trading days (daily) or 52 weeks (weekly).",
            "Correlations use weekly returns by default. The series are observed at "
            "different times (ECB fixing 14:10 CET; Tokyo closes 06:00 UTC, New York "
            "20:00 UTC), and daily returns of asynchronous series understate correlation "
            "(the Epps effect); weekly returns largely remove the timing mismatch. Each "
            "estimate shows its number of returns and a 95 % confidence interval from the "
            "Fisher transform, tanh(atanh(ρ) ± 1.96 / √(n − 3)).",
            "For a quanto, the correlation needed is between the asset and the FX rate "
            "quoted as payment currency per unit of the asset's currency — e.g. a CAC 40 "
            "option paid in USD needs corr(CAC 40, EUR/USD).",
        ],
    ),
]
