"""Rough Bergomi calibration on the stored market surface (etc/roadmap.md §1c).

From what market_snapshot.local_vol_market built out of the database (never a
live source): the SVI slices the Dupire grid is made of (surface_slices).

- ξ0(t) = E[V_t], the forward variance curve, is the market's: the
  variance-swap total variance of each slice, w_VS(T) = −2 E[ln(S_T / F)],
  replicated from the whole SVI smile by out-of-the-money options (Carr &
  Madan 1998), differenced in T. It is not fitted. The at-the-money variance
  would not do: with a negative skew the variance swap sits well above it,
  and a model on the ATM curve comes out a vol point low everywhere
  (measured on SPY).
- (H, η, ρ) are fitted by Levenberg-Marquardt to implied vols read off the
  same slices at standardised moneynesses (the Heston targets' grid), over
  the short end where roughness shows: maturities from MIN_TTM to MAX_TTM.
  Every objective call prices the whole surface by Monte-Carlo on the same
  paths (C++ market/rough_bergomi_calibration.hpp): the fit is deterministic
  for a snapshot, and its Monte-Carlo error is reported per quote.

One calibration per (ticker, snapshot, rate), cached in process and stored
(storage.py) under a digest of everything it reads, like stochastic_vol.py.
"""

from __future__ import annotations

import dataclasses
import hashlib
import json
import logging
import math
import time
from dataclasses import dataclass
from datetime import date
from typing import List, Optional, Tuple

import quantmodeling as qm

from .keyed_cache import KeyedCache
from .logging_utils import LOGGER_NAME
from .market_snapshot import LocalVolMarket
from .storage import get_storage
from .telemetry import tracer

#: Maturities the fit sees: one week to six months.
MIN_TTM = 0.02
MAX_TTM = 0.5
#: Moneynesses per maturity, in standard deviations of the ATM vol.
Z_POINTS = (-2.0, -1.5, -1.0, -0.5, 0.0, 0.5, 1.0)
#: Monte-Carlo of each objective call: paths (antithetic pairs count two),
#: hybrid-scheme steps a year, and a fixed seed (a snapshot always gives the
#: same fit, so a replay reproduces a price).
N_PATHS = 20_000
STEPS_PER_YEAR = 400
SEED = 1
METHOD = 1
PREFIX = "calibrations/rough-bergomi"

_log = logging.getLogger(LOGGER_NAME)


class RoughCalibrationUnavailable(RuntimeError):
    """The surface does not support a rough Bergomi calibration."""


@dataclass(frozen=True)
class RoughVol:
    ticker: str
    snapshot: date
    H: float
    eta: float
    rho: float
    xi_times: Tuple[float, ...]
    xi_values: Tuple[float, ...]
    #: Forward variances floored (calendar arbitrage in the ATM vols).
    xi_floored: int
    iv_rmse: float
    iv_worst: float
    n_quotes: int
    n_maturities: int
    #: Largest Monte-Carlo error of a model vol: the resolution of the fit.
    mc_vol_error: float
    seconds: float


def _w(sl: dict, k: float) -> float:
    x = k - sl["m"]
    return sl["a"] + sl["b"] * (sl["rho"] * x + math.sqrt(x * x + sl["sigma"] ** 2))


def _norm_cdf(x: float) -> float:
    return 0.5 * math.erfc(-x / math.sqrt(2.0))


def _black_otm(k: float, w: float) -> float:
    """Forward-normalised Black price of the out-of-the-money option at
    log-moneyness k, total variance w (F = 1, no discounting)."""
    sd = math.sqrt(w)
    d1 = -k / sd + 0.5 * sd
    d2 = d1 - sd
    if k >= 0.0:
        return _norm_cdf(d1) - math.exp(k) * _norm_cdf(d2)
    return math.exp(k) * _norm_cdf(-d2) - _norm_cdf(-d1)


def variance_swap_total_variance(sl: dict) -> float:
    """w_VS(T) = 2 ∫ OTM(K) / K² dK over the slice's smile, in k = ln K/F:
    2 ∫ OTM(k) e^{-k} dk, Simpson on ±8 ATM standard deviations (the
    integrand is negligible beyond). n is even, so the kink at k = 0, where
    OTM switches from put to call, is a node and each side stays O(h^4)."""
    sd = math.sqrt(max(_w(sl, 0.0), 1e-12))
    lo, hi, n = -8.0 * sd, 8.0 * sd, 4000
    h = (hi - lo) / n
    total = 0.0
    for i in range(n + 1):
        k = lo + i * h
        w = _w(sl, k)
        f = _black_otm(k, w) * math.exp(-k) if w > 0.0 else 0.0
        total += f * (1.0 if i in (0, n) else (4.0 if i % 2 else 2.0))
    return 2.0 * total * h / 3.0


def targets(
    market: LocalVolMarket,
) -> Tuple[List[Tuple[float, float, float]], List[float], List[float]]:
    """(targets [(T, k, vol)], maturities, variance-swap total variances):
    the slices from the first up to MAX_TTM feed ξ0; those from MIN_TTM on
    are targets, only where each slice was quoted (#97)."""
    out: List[Tuple[float, float, float]] = []
    atm_t: List[float] = []
    atm_w: List[float] = []
    # The Dupire grid's slices only (in_surface, #119): one surface for
    # every model.
    kept = [s for s in market.svi_slices if s.get("in_surface", True)]
    for sl in sorted(kept, key=lambda s: s["ttm"]):
        T = float(sl["ttm"])
        if T > MAX_TTM:
            break
        w0 = _w(sl, 0.0)
        if not w0 > 0.0:
            continue
        atm_t.append(T)
        atm_w.append(variance_swap_total_variance(sl))
        if T < MIN_TTM:
            continue
        sigma_atm = math.sqrt(w0 / T)
        quoted = [q[0] for q in (sl.get("quotes") or [])]
        k_lo, k_hi = (min(quoted), max(quoted)) if quoted else (-math.inf, math.inf)
        for z in Z_POINTS:
            k = z * sigma_atm * math.sqrt(T)
            if k_lo <= k <= k_hi:
                w = _w(sl, k)
                if w > 0.0:
                    out.append((T, k, math.sqrt(w / T)))
    return out, atm_t, atm_w


def _calibrate(market: LocalVolMarket) -> RoughVol:
    start = time.perf_counter()
    tgt, atm_t, atm_w = targets(market)
    if len({t for t, _, _ in tgt}) < 2:
        raise RoughCalibrationUnavailable(
            f"the {market.ticker} surface of {market.valuation_date.isoformat()} has "
            f"fewer than two maturities between {MIN_TTM:g} and {MAX_TTM:g} years: not "
            "enough short-dated term structure to fit rough Bergomi"
        )
    fit = qm.calibrate_rough_bergomi(
        tgt,
        atm_t,
        atm_w,
        n_paths=N_PATHS,
        steps_per_year=STEPS_PER_YEAR,
        seed=SEED,
    )
    errors = [e for e in fit["model_vol_errors"] if e is not None and math.isfinite(e)]
    return RoughVol(
        ticker=market.ticker,
        snapshot=market.valuation_date,
        H=float(fit["H"]),
        eta=float(fit["eta"]),
        rho=float(fit["rho"]),
        xi_times=tuple(fit["xi_times"]),
        xi_values=tuple(fit["xi_values"]),
        xi_floored=int(fit["xi_floored"]),
        iv_rmse=float(fit["iv_rmse"]),
        iv_worst=float(fit["iv_worst"]),
        n_quotes=int(fit["n_quotes"]),
        n_maturities=len({t for t, _, _ in tgt}),
        mc_vol_error=max(errors) if errors else 0.0,
        seconds=time.perf_counter() - start,
    )


_cache: KeyedCache[RoughVol] = KeyedCache(16)


def store_key(market: LocalVolMarket) -> str:
    payload = json.dumps(
        {
            "method": METHOD,
            "paths": N_PATHS,
            "steps": STEPS_PER_YEAR,
            "seed": SEED,
            "ticker": market.ticker,
            "snapshot": market.valuation_date.isoformat(),
            "slices": market.svi_slices,
        },
        sort_keys=True,
        default=list,
    )
    return hashlib.sha256(payload.encode()).hexdigest()[:32]


def _load(key: str) -> Optional[RoughVol]:
    try:
        raw = get_storage().read_json(f"{PREFIX}/{key}")
    except Exception:  # noqa: BLE001 -- a miss, not an error
        return None
    if not raw:
        return None
    try:
        raw["snapshot"] = date.fromisoformat(raw["snapshot"])
        raw["xi_times"] = tuple(raw["xi_times"])
        raw["xi_values"] = tuple(raw["xi_values"])
        return RoughVol(**raw)
    except (KeyError, TypeError, ValueError):
        return None


def _save(key: str, rv: RoughVol) -> None:
    raw = dataclasses.asdict(rv)
    raw["snapshot"] = rv.snapshot.isoformat()
    raw["xi_times"] = list(rv.xi_times)
    raw["xi_values"] = list(rv.xi_values)
    try:
        get_storage().write_json(f"{PREFIX}/{key}", raw)
    except Exception:  # noqa: BLE001 -- the next restart recalibrates
        _log.exception("could not store the rough Bergomi calibration")


def calibrate(market: LocalVolMarket) -> RoughVol:
    """Rough Bergomi for this market surface: from the process cache, else
    the store, else calibrated (and stored)."""
    key = store_key(market)

    def compute() -> RoughVol:
        stored = _load(key)
        if stored is not None:
            return stored
        with tracer.start_as_current_span(
            "calibration.rough_bergomi",
            attributes={"qm.valuation_date": str(market.valuation_date)},
        ):
            try:
                result = _calibrate(market)
            except (RuntimeError, ValueError) as exc:
                if isinstance(exc, RoughCalibrationUnavailable):
                    raise
                raise RoughCalibrationUnavailable(str(exc)) from exc
        _save(key, result)
        return result

    return _cache.get(key, compute)


__all__ = ["RoughCalibrationUnavailable", "RoughVol", "calibrate", "targets"]
