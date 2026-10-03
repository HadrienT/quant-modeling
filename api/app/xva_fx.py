"""The second currency of the xVA page: a EUR rate model and the EUR/USD
exchange rate, on market data (blueprint/wp/23-xva.md §14.15, lot X10).

Everything is read from the store, with no fallback:

- the **Euribor swap curve** and the **EUR swaption vols** Hull-White is
  calibrated on, from the trades DTCC publishes, exactly as the USD ones
  (`swaption_market`): one curve both discounts and projects;
- the **spot**, the ECB's euro reference rate;
- the **volatility of the exchange rate**, from the at-the-money straddles
  traded on EUR/USD (`fx_option_market`): the model's spot volatility is the
  one that gives the option of the calibration expiry its market volatility;
- the three **correlations** — between the two rates, and of each with the
  exchange rate — which no free market price implies: they are historical
  estimates on weekly changes, each with its confidence interval, and are
  shown as estimates.
"""

from __future__ import annotations

import math
from dataclasses import dataclass
from datetime import date, timedelta
from typing import Dict, List, Tuple

import pandas as pd
import quantmodeling as qm

from . import db, fx, fx_option_market, rates_derivatives, swaption_market

DOMESTIC = "USD"
FOREIGN = "EUR"
#: The swap EUR swaptions are written on, as DTCC names it.
EURIBOR_SWAP = "NA/Swap Fxd Flt EUR"
CURVE_LABEL = "EUR Euribor"
#: The pair as DTCC names it.
PAIR = "EUR USD"
#: The spot volatility is calibrated on the longest expiry with at least this
#: many straddles behind it.
MIN_CALIBRATION_STRADDLES = 10
#: Rate series the correlations are estimated on: five-year yields, the
#: middle of the curve the trades live on.
USD_RATE_SERIES = "DGS5"
EUR_RATE_SERIES = "EUR.AAA_SPOT_5Y"
CORRELATION_YEARS = 3
_MIN_WEEKS = 52


class TwoCurrencyUnavailable(RuntimeError):
    """An input the two-currency model needs is not in the store."""


@dataclass(frozen=True)
class ForeignRates:
    as_of: date
    label: str
    times: List[float]
    dfs: List[float]
    mean_reversion: float
    sigma: float
    rmse_bp: float
    vol_points: int
    swaption_trades: int


@dataclass(frozen=True)
class Estimate:
    """A historical correlation with its 95 % confidence interval."""

    value: float
    low: float
    high: float
    weeks: int


@dataclass(frozen=True)
class FxVolatility:
    expiry: float
    market: float
    low: float
    high: float
    straddles: int
    model: float


@dataclass(frozen=True)
class TwoCurrencyMarket:
    rates: ForeignRates
    spot: float
    spot_as_of: date
    spot_volatility: float
    calibration_expiry: float
    fx_volatilities: List[FxVolatility]
    fx_window_start: date
    domestic_foreign: Estimate
    domestic_fx: Estimate
    foreign_fx: Estimate
    correlation_start: date
    correlation_end: date

    def correlations(self) -> Tuple[float, float, float]:
        return (
            self.domestic_foreign.value,
            self.domestic_fx.value,
            self.foreign_fx.value,
        )

    def discount(self, t: float) -> float:
        """P_EUR(0, t) on the curve the engine uses: a constant forward rate
        before the first pillar, where `qm.discount_factors` would be flat."""
        return qm.rate_curve_discount_factors(self.rates.times, self.rates.dfs, [t])[0]

    def argument(self) -> dict:
        """What qm.xva_netting_set takes as `foreign`."""
        return {
            "currency": FOREIGN,
            "domestic_currency": DOMESTIC,
            "discount_times": self.rates.times,
            "discount_factors": self.rates.dfs,
            "hull_white": (self.rates.mean_reversion, self.rates.sigma),
            "spot": self.spot,
            "fx_volatility": self.spot_volatility,
            "correlations": self.correlations(),
        }


def _foreign_rates(as_of: date) -> ForeignRates:
    """The Euribor curve of `as_of` and Hull-White calibrated on the EUR
    swaptions of the window before it."""
    window = rates_derivatives.MARKET_WINDOW_DAYS
    curves = db.dtcc_swap_curves(EURIBOR_SWAP, as_of - timedelta(days=window))
    complete = {
        d: rates for d, rates in curves.items() if len(rates) >= 5 and d <= as_of
    }
    if as_of not in complete:
        raise TwoCurrencyUnavailable(
            f"No Euribor swap curve for {as_of.isoformat()} in the store (run "
            "data-ingest's dtcc-swap-rates source)"
        )
    window_start = as_of - timedelta(days=window)
    trades = [
        t
        for t in db.dtcc_swaption_trades(EURIBOR_SWAP, window_start)
        if t.report_date <= as_of
    ]
    grid = swaption_market.atm_normal_vols(trades, complete)
    if len(grid.points) < 3:
        raise TwoCurrencyUnavailable(
            f"Only {len(grid.points)} EUR swaption vol points could be built from the "
            f"{len(trades)} trades reported since {window_start.isoformat()}: not "
            "enough to calibrate a model"
        )
    quotes = [(float(r.tenor_years), r.rate) for r in complete[as_of]]
    curve = qm.bootstrap_ois_curve([], quotes)
    t, d = list(curve["times"]), list(curve["discount_factors"])
    vols = [(p.expiry, float(p.tenor), p.normal_vol) for p in grid.points]
    hw = qm.calibrate_hull_white(t, d, t, d, vols, 1, 1, None)
    return ForeignRates(
        as_of,
        f"{CURVE_LABEL}, from the swaps and swaptions traded and published by DTCC: "
        f"swap curve of {as_of.isoformat()}, swaption vols from {grid.trades_used} "
        f"trades since {window_start.isoformat()}. Medians of trades, not dealer quotes.",
        t,
        d,
        hw["mean_reversion"],
        hw["sigma"],
        hw["rmse_bp"],
        len(vols),
        grid.trades_used,
    )


def _weekly(series: pd.Series) -> pd.Series:
    s = series.sort_index()
    s.index = pd.to_datetime(s.index)
    return s.resample("W-FRI").last().dropna()


def _correlation(a: pd.Series, b: pd.Series) -> Estimate:
    """Pearson correlation of two series of weekly changes on their common
    weeks, with the 95 % interval of the Fisher transform."""
    joined = pd.concat([a, b], axis=1, join="inner").dropna()
    n = len(joined)
    if n < _MIN_WEEKS:
        raise TwoCurrencyUnavailable(
            f"Only {n} common weeks of history to estimate a correlation on: too few"
        )
    rho = float(joined.iloc[:, 0].corr(joined.iloc[:, 1]))
    z = math.atanh(max(min(rho, 0.999999), -0.999999))
    half = 1.959964 / math.sqrt(n - 3)
    return Estimate(rho, math.tanh(z - half), math.tanh(z + half), n)


def _correlations(as_of: date) -> Tuple[Estimate, Estimate, Estimate, date, date]:
    """(rates together, USD rate with the exchange rate, EUR rate with it).
    The exchange rate is in dollars per euro: a positive correlation with the
    dollar rate means the euro rises when dollar rates do."""
    since = as_of - timedelta(days=365 * CORRELATION_YEARS)
    usd = db.fred_series(USD_RATE_SERIES, since)
    eur_rows = db.rates_history("intl", EUR_RATE_SERIES, since)
    try:
        spot = fx.pair_history(FOREIGN, DOMESTIC, since)
    except fx.FxUnavailable as exc:
        raise TwoCurrencyUnavailable(str(exc)) from exc
    if usd.empty or not eur_rows:
        raise TwoCurrencyUnavailable(
            f"No history of {USD_RATE_SERIES} or {EUR_RATE_SERIES} in the store to "
            "estimate the correlations on (data-ingest's fred-macro and intl-rates)"
        )
    eur = pd.Series([v for _, v in eur_rows], index=[d for d, _ in eur_rows])
    # Rates move by differences, the exchange rate by log returns.
    d_usd = _weekly(usd[usd.index <= pd.Timestamp(as_of)]).diff().dropna()
    d_eur = _weekly(eur).diff().dropna()
    d_fx = _weekly(spot).map(math.log).diff().dropna()
    rates_together = _correlation(d_usd, d_eur)
    common = pd.concat([d_usd, d_eur, d_fx], axis=1, join="inner").dropna()
    return (
        rates_together,
        _correlation(d_usd, d_fx),
        _correlation(d_eur, d_fx),
        common.index[0].date(),
        common.index[-1].date(),
    )


def market(
    as_of: date,
    usd_times: List[float],
    usd_dfs: List[float],
    usd_hull_white: Tuple[float, float],
) -> TwoCurrencyMarket:
    """The EUR side and the exchange rate on the date of the USD curve.

    No fallback: a missing curve, too few swaptions or straddles, or too
    short a history is an explicit TwoCurrencyUnavailable.
    """
    rates = _foreign_rates(as_of)
    try:
        history = fx.pair_history(FOREIGN, DOMESTIC, as_of - timedelta(days=60))
    except fx.FxUnavailable as exc:
        raise TwoCurrencyUnavailable(str(exc)) from exc
    history = history[[d <= as_of for d in history.index]]
    if history.empty:
        raise TwoCurrencyUnavailable("No EUR/USD reference rate in the store")
    spot_as_of = history.index[-1]
    if (as_of - spot_as_of).days > rates_derivatives.MARKET_STALE_AFTER_DAYS:
        raise TwoCurrencyUnavailable(
            f"The latest EUR/USD reference rate is from {spot_as_of.isoformat()}: "
            "data-ingest's ecb-fx source has not run"
        )
    spots: Dict[date, float] = {d: float(v) for d, v in history.items()}

    window_start = as_of - timedelta(days=rates_derivatives.MARKET_WINDOW_DAYS)
    trades = [
        t
        for t in db.dtcc_fx_option_trades(PAIR, window_start)
        if t.report_date <= as_of
    ]
    grid = fx_option_market.atm_volatilities(
        trades,
        FOREIGN,
        DOMESTIC,
        spots,
        lambda t: qm.rate_curve_discount_factors(usd_times, usd_dfs, [t])[0],
    )
    liquid = [p for p in grid.points if p.trades >= MIN_CALIBRATION_STRADDLES]
    if not liquid:
        raise TwoCurrencyUnavailable(
            f"No EUR/USD expiry with {MIN_CALIBRATION_STRADDLES} at-the-money straddles "
            f"among the {len(trades)} option trades reported since "
            f"{window_start.isoformat()} (run data-ingest's dtcc-fx-options source)"
        )
    target = max(liquid, key=lambda p: p.expiry)

    rates_together, usd_fx, eur_fx, start, end = _correlations(as_of)
    rho = (rates_together.value, usd_fx.value, eur_fx.value)
    try:
        fit = qm.cross_currency_fx_volatility(
            usd_hull_white,
            (rates.mean_reversion, rates.sigma),
            rho,
            target.expiry,
            target.volatility,
            [p.expiry for p in grid.points],
        )
    except RuntimeError as exc:
        raise TwoCurrencyUnavailable(str(exc)) from exc
    return TwoCurrencyMarket(
        rates=rates,
        spot=float(history.iloc[-1]),
        spot_as_of=spot_as_of,
        spot_volatility=fit["spot_volatility"],
        calibration_expiry=target.expiry,
        fx_volatilities=[
            FxVolatility(p.expiry, p.volatility, p.low, p.high, p.trades, model)
            for p, model in zip(grid.points, fit["implied_volatilities"])
        ],
        fx_window_start=window_start,
        domestic_foreign=rates_together,
        domestic_fx=usd_fx,
        foreign_fx=eur_fx,
        correlation_start=start,
        correlation_end=end,
    )


def par_coupon(times: List[float], dfs: List[float], maturity: float) -> float:
    """The annual coupon that makes a bond of this curve worth par."""
    years = [float(k) for k in range(1, int(round(maturity)) + 1)]
    discounts = qm.rate_curve_discount_factors(times, dfs, years)
    return (1.0 - discounts[-1]) / sum(discounts)
