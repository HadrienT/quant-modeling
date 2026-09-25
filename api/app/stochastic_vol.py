"""Stochastic-volatility calibration on the stored market surface: Heston,
then the stochastic-local-vol leverage on top of it.

Both start from what market_snapshot.local_vol_market has already built from
the database (never a live source): the SVI slices fitted to the stored
option chain and the Dupire grid derived from them.

1. Heston (v0, kappa, theta, xi, rho) is fitted to the SVI surface, read at a
   fixed set of standardised moneynesses per maturity (C++
   market/heston_calibration.hpp: Levenberg-Marquardt on COS prices,
   vega-scaled). Fitting the smoothed surface rather than the raw quotes is
   the usual desk practice: the quotes have already been cleaned and made
   arbitrage-free once, and the fit sees the same surface local vol does.
   Maturities under MIN_TTM are left out: five parameters cannot follow the
   steepest short-dated skew, and the leverage below reprices it anyway.
2. The SLV leverage L(K, T) is calibrated by the particle method so that the
   Heston dynamics times L reproduce the Dupire grid's marginals (C++
   market/slv_calibration.hpp).

One calibration per (ticker, snapshot, rate): the snapshot fixes every input.
It costs a few seconds, so it is cached in process.
"""

from __future__ import annotations

import math
import threading
import time
from collections import OrderedDict
from dataclasses import dataclass
from datetime import date
from typing import Dict, List, Tuple

import quantmodeling as qm

from .market_snapshot import LocalVolMarket
from .telemetry import tracer

#: Shortest maturity the Heston fit sees (about 18 days).
MIN_TTM = 0.05
#: Moneynesses per maturity, in standard deviations of the ATM vol:
#: k = z * sigma_atm * sqrt(T), the range listed equity chains quote densely
#: (further on the put side, where equity skew lives).
Z_POINTS = (-2.0, -1.5, -1.0, -0.5, 0.0, 0.5, 1.0, 1.5)
#: Particles of the leverage calibration, and its seed (fixed: the same
#: snapshot always gives the same leverage, so a replay reproduces a price).
SLV_PARTICLES = 50_000
SLV_SEED = 1
_CACHE_SIZE = 16

HESTON_KEYS = ("v0", "kappa", "theta", "xi", "rho")


class CalibrationUnavailable(RuntimeError):
    """The surface does not support a stochastic-vol calibration."""


@dataclass(frozen=True)
class StochasticVol:
    ticker: str
    snapshot: date
    heston: Dict[str, float]
    #: The Heston fit, exact implied-vol errors (vol points).
    iv_rmse: float
    iv_worst: float
    n_quotes: int
    n_maturities: int
    feller: bool
    #: The leverage, K-major on the Dupire grid's (K_grid, T_grid).
    leverage_flat: Tuple[float, ...]
    leverage_min: float
    leverage_max: float
    #: Share of leverage points held at the calibration's floor or cap: where
    #: E[v | S] could not be estimated (too few particles in the bucket), the
    #: marginals there are not the Dupire surface's.
    leverage_clamped_share: float
    n_particles: int
    seconds: float


def _svi_w(sl: dict, k: float) -> float:
    x = k - sl["m"]
    return sl["a"] + sl["b"] * (sl["rho"] * x + math.sqrt(x * x + sl["sigma"] ** 2))


def heston_targets(
    market: LocalVolMarket,
) -> Tuple[List[float], List[float], List[float]]:
    """(strikes, ttms, implied vols) read off the SVI slices, k = ln(K/F_T)
    at the snapshot's spot, rate and dividend (the calibration's own
    convention, see LocalVolMarket.svi_slices)."""
    strikes: List[float] = []
    ttms: List[float] = []
    vols: List[float] = []
    for sl in market.svi_slices:
        T = float(sl["ttm"])
        if T < MIN_TTM:
            continue
        w_atm = _svi_w(sl, 0.0)
        if not w_atm > 0.0:
            continue
        forward = market.spot * math.exp((market.rate - market.dividend) * T)
        sigma_atm = math.sqrt(w_atm / T)
        for z in Z_POINTS:
            k = z * sigma_atm * math.sqrt(T)
            w = _svi_w(sl, k)
            if w > 0.0:
                strikes.append(forward * math.exp(k))
                ttms.append(T)
                vols.append(math.sqrt(w / T))
    return strikes, ttms, vols


def _calibrate(market: LocalVolMarket) -> StochasticVol:
    start = time.perf_counter()
    strikes, ttms, vols = heston_targets(market)
    if len({t for t in ttms}) < 2:
        raise CalibrationUnavailable(
            f"the {market.ticker} surface of {market.valuation_date.isoformat()} "
            f"has fewer than two maturities beyond {MIN_TTM:g} years: not enough "
            "term structure to fit Heston"
        )
    fit = qm.calibrate_heston(
        strikes, ttms, vols, market.spot, market.rate, market.dividend
    )
    heston = {k: float(fit[k]) for k in HESTON_KEYS}
    lev = qm.calibrate_slv_leverage(
        market.spot,
        market.rate,
        market.dividend,
        heston,
        market.K_grid,
        market.T_grid,
        market.sigma_loc_flat,
        n_particles=SLV_PARTICLES,
        seed=SLV_SEED,
    )
    L = lev["leverage_flat"]
    floor, cap = lev["leverage_floor"], lev["leverage_cap"]
    clamped = sum(1 for x in L if x <= floor * (1 + 1e-12) or x >= cap * (1 - 1e-12))
    return StochasticVol(
        ticker=market.ticker,
        snapshot=market.valuation_date,
        heston=heston,
        iv_rmse=float(fit["iv_rmse"]),
        iv_worst=float(fit["iv_worst"]),
        n_quotes=int(fit["n_quotes"]),
        n_maturities=int(fit["n_maturities"]),
        feller=bool(fit["feller"]),
        leverage_flat=tuple(L),
        leverage_min=min(L),
        leverage_max=max(L),
        leverage_clamped_share=clamped / len(L),
        n_particles=int(lev["n_particles"]),
        seconds=time.perf_counter() - start,
    )


_cache: "OrderedDict[tuple, StochasticVol]" = OrderedDict()
_locks: Dict[tuple, threading.Lock] = {}
_guard = threading.Lock()


def calibrate(market: LocalVolMarket) -> StochasticVol:
    """Heston and SLV leverage for this market surface, cached per
    (ticker, snapshot, rate); two concurrent requests for the same surface
    calibrate it once."""
    key = (market.ticker, market.valuation_date, round(market.rate, 10))
    with _guard:
        if key in _cache:
            _cache.move_to_end(key)
            return _cache[key]
        lock = _locks.setdefault(key, threading.Lock())
    with lock:
        with _guard:
            if key in _cache:
                return _cache[key]
        with tracer.start_as_current_span(
            "calibration.stochastic_vol",
            attributes={"qm.valuation_date": str(market.valuation_date)},
        ):
            try:
                result = _calibrate(market)
            except (RuntimeError, ValueError) as exc:
                if isinstance(exc, CalibrationUnavailable):
                    raise
                raise CalibrationUnavailable(str(exc)) from exc
        with _guard:
            _cache[key] = result
            while len(_cache) > _CACHE_SIZE:
                _cache.popitem(last=False)
            _locks.pop(key, None)
        return result


__all__ = [
    "CalibrationUnavailable",
    "HESTON_KEYS",
    "MIN_TTM",
    "StochasticVol",
    "Z_POINTS",
    "calibrate",
    "heston_targets",
]
