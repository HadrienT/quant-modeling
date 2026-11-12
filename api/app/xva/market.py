"""Market inputs of the computation: rates, credit curves, historical dynamics."""

from __future__ import annotations

from dataclasses import dataclass
from datetime import date
from typing import TYPE_CHECKING, List, Optional, Tuple

import quantmodeling as qm

from .. import credit, db, rates_derivatives

if TYPE_CHECKING:  # pragma: no cover — xva_equity imports the portfolio modules
    pass
from ..xva_schemas import (
    CreditInput,
    HistoricalDynamics,
    XvaRequest,
)
from .base import HISTORY_SERIES, HISTORY_SINCE, XvaUnavailable, _BUSINESS_DAYS
from .demos import _RATING_SERIES


@dataclass(frozen=True)
class _Rates:
    as_of: date
    label: str
    times: List[float]
    dfs: List[float]
    mean_reversion: float
    sigma: float
    rmse_bp: float
    vol_points: int
    swaption_trades: int
    quotes: List[Tuple[float, float]]
    vols: List[Tuple[float, float, float]]


def _rates_market(today: Optional[date]) -> _Rates:
    try:
        m = rates_derivatives.market_request(today)
    except rates_derivatives.RatesMarketUnavailable as exc:
        raise XvaUnavailable(str(exc)) from exc
    quotes = [(q["tenor"], q["rate"]) for q in m["ois"]]
    vols = [(v["expiry"], v["tenor"], v["normal_vol"]) for v in m["swaption_vols"]]
    curve = qm.bootstrap_ois_curve([], quotes)
    t, d = curve["times"], curve["discount_factors"]
    hw = qm.calibrate_hull_white(t, d, t, d, vols, 1, 1, None)
    return _Rates(
        m["as_of"],
        m["label"],
        list(t),
        list(d),
        hw["mean_reversion"],
        hw["sigma"],
        hw["rmse_bp"],
        len(vols),
        m["trades_used"],
        quotes,
        vols,
    )


def _credit(
    rating: str, recovery: float, rates: _Rates, warnings: List[str]
) -> CreditInput:
    """The flat hazard rate that reprices a five-year CDS at the rating's
    spread, as on the credit page."""
    snap = db.rates_curve_snapshot("fred", list(_RATING_SERIES.values()))
    if snap is None:
        raise XvaUnavailable(
            "No date on which every ICE BofA rating spread is in the store "
            "(run data-ingest's fred-macro source)"
        )
    as_of, values = snap
    stale = credit.staleness_warning(as_of, "credit spreads by rating")
    if stale and stale not in warnings:
        warnings.append(stale)
    spread = values[_RATING_SERIES[rating]] / 100.0
    boot = qm.bootstrap_credit_curve(
        [(credit.RATING_HAZARD_MATURITY, spread)], rates.times, rates.dfs, recovery
    )
    return CreditInput(
        rating=rating, spread=spread, hazard=boot["hazards"][0], as_of=as_of
    )


def _historical(req: XvaRequest) -> HistoricalDynamics:
    series = db.fred_series(HISTORY_SERIES, HISTORY_SINCE)
    if len(series) < 500:
        raise XvaUnavailable(
            f"The history of {HISTORY_SERIES} is not in the store "
            "(run data-ingest's fred-macro source with --full)"
        )
    values = [float(v) / 100.0 for v in series.to_list()]
    dt = 1.0 / _BUSINESS_DAYS
    sigma = qm.estimate_historical_volatility(values, dt)
    a = theta = a_error = theta_error = None
    try:
        e = qm.estimate_historical_rate_dynamics(values, dt)
        a, theta, sigma = e["mean_reversion"], e["long_run_rate"], e["sigma"]
        a_error, theta_error = (
            e["mean_reversion_std_error"],
            e["long_run_rate_std_error"],
        )
    except RuntimeError:
        pass  # no measurable mean reversion: a and θ must come from the request
    h = req.historical
    if (a is None and h.mean_reversion is None) or (
        theta is None and h.long_run_rate is None
    ):
        raise XvaUnavailable(
            f"No mean reversion can be measured on {HISTORY_SERIES} since "
            f"{HISTORY_SINCE.isoformat()}: set the mean reversion and the long-run rate"
        )
    overridden = [
        name
        for name, given in (
            ("mean_reversion", h.mean_reversion),
            ("long_run_rate", h.long_run_rate),
            ("sigma", h.sigma),
        )
        if given is not None
    ]
    return HistoricalDynamics(
        mean_reversion=h.mean_reversion if h.mean_reversion is not None else a,
        long_run_rate=h.long_run_rate if h.long_run_rate is not None else theta,
        sigma=h.sigma if h.sigma is not None else sigma,
        estimated_mean_reversion=a,
        estimated_long_run_rate=theta,
        estimated_sigma=sigma,
        mean_reversion_std_error=a_error,
        long_run_rate_std_error=theta_error,
        series=HISTORY_SERIES,
        since=series.index[0].date(),
        observations=len(values),
        overridden=overridden,
    )
