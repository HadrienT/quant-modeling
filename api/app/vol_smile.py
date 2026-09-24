"""The implied-volatility smile of an underlying on a date, from the stored
option chains — what a desk marks its equity derivatives against.

One calibration per (ticker, option-chain snapshot): market_snapshot.py
cleans the stored quotes and fits one SVI slice per maturity (Gatheral's raw
SVI, total variance w(k) = a + b(ρ(k − m) + √((k − m)² + σ²)) at log-
moneyness k = ln(K / F_T)), then builds the Dupire local-vol grid from them.
Both are kept:

- `implied_vol(K, T)` reads the SVI surface the way the C++ SVISurface does
  (market/svi_surface.hpp): total variance linear in T at fixed k between the
  two bracketing slices. Beyond the calibrated maturities the implied vol is
  held flat in T at the nearest slice's (the C++ class holds total variance
  flat instead, which blows the vol up as T → 0 — not a mark anyone would
  quote). Sticky strike: k is measured at the snapshot's forward, so a
  strike keeps its vol when spot moves between snapshots.
- `K_grid / T_grid / sigma_loc_flat` feed the local-vol Monte-Carlo (the
  scripting engine's `local_vol` model).

A smile exists only where data-ingest stores option chains (a fixed US
universe) and only for dates within market_snapshot.MAX_SNAPSHOT_GAP_DAYS of
a snapshot; everywhere else there is none, and the caller says so.
"""

from __future__ import annotations

import bisect
import math
from dataclasses import dataclass
from datetime import date
from functools import lru_cache
from typing import Callable, Dict, List, Optional

from . import db, market_snapshot


@dataclass(frozen=True)
class Smile:
    ticker: str
    snapshot: date
    spot: float  # the close the calibration used
    rate: float
    dividend: float
    slices: tuple  # of dicts: ttm, a, b, rho, m, sigma, rmse, converged, ...
    K_grid: tuple
    T_grid: tuple
    sigma_loc_flat: tuple

    @property
    def ttms(self) -> List[float]:
        return [s["ttm"] for s in self.slices]

    def forward(self, T: float) -> float:
        return self.spot * math.exp((self.rate - self.dividend) * T)

    @staticmethod
    def _w(sl: dict, k: float) -> float:
        x = k - sl["m"]
        return sl["a"] + sl["b"] * (sl["rho"] * x + math.sqrt(x * x + sl["sigma"] ** 2))

    def implied_vol(self, K: float, T: float) -> float:
        """Black-Scholes implied vol at strike K and maturity T (years)."""
        ttms = self.ttms
        Tc = min(max(T, ttms[0]), ttms[-1])
        k = math.log(K / self.forward(Tc))
        if len(ttms) == 1:
            w = self._w(self.slices[0], k)
        else:
            i = min(max(bisect.bisect_right(ttms, Tc) - 1, 0), len(ttms) - 2)
            T0, T1 = ttms[i], ttms[i + 1]
            lam = (Tc - T0) / (T1 - T0)
            w = (1 - lam) * self._w(self.slices[i], k) + lam * self._w(
                self.slices[i + 1], k
            )
        return math.sqrt(max(w, 0.0) / Tc)

    def dvol_dK(self, K: float, T: float) -> float:
        """Skew ∂σ/∂K at fixed T, by a central difference of 0.1 % of K."""
        h = 1e-3 * K
        return (self.implied_vol(K + h, T) - self.implied_vol(K - h, T)) / (2 * h)

    def fit_quality(self) -> Dict[str, float]:
        rmses = [s.get("rmse", float("nan")) for s in self.slices]
        return {
            "slices": float(len(self.slices)),
            "max_rmse": max(rmses) if rmses else float("nan"),
            "converged": float(sum(1 for s in self.slices if s.get("converged"))),
        }


@lru_cache(maxsize=64)
def _calibrated(ticker: str, snapshot: date, rate: float) -> Optional[Smile]:
    try:
        m = market_snapshot.local_vol_market(ticker, rate, snapshot)
    except market_snapshot.MarketDataUnavailable:
        return None
    slices = sorted(m.svi_slices, key=lambda s: s["ttm"])
    if not slices:
        return None
    return Smile(
        ticker=ticker,
        snapshot=m.valuation_date,
        spot=m.spot,
        rate=rate,
        dividend=m.dividend,
        slices=tuple(slices),
        K_grid=tuple(m.K_grid),
        T_grid=tuple(m.T_grid),
        sigma_loc_flat=tuple(m.sigma_loc_flat),
    )


def snapshot_for(ticker: Optional[str], d: date) -> Optional[date]:
    """The option-chain snapshot valid on `d`: the latest stored on or
    before it, at most MAX_SNAPSHOT_GAP_DAYS old; None where there is none."""
    if not ticker:
        return None
    try:
        snap = db.options_snapshot_date_on_or_before(ticker.upper(), d)
    except db.StoreUnavailable:
        return None
    if snap is None or (d - snap).days > market_snapshot.MAX_SNAPSHOT_GAP_DAYS:
        return None
    return snap


def smile_for(
    ticker: Optional[str], d: date, rate_at: Callable[[date], float]
) -> Optional[Smile]:
    """The smile valid on `d`. One calibration per (ticker, snapshot): its
    forwards use `rate_at(snapshot)` — the one-year zero rate of the
    currency's curve on the snapshot date — whatever the option being
    marked, so the surface does not depend on who asks for it."""
    snap = snapshot_for(ticker, d)
    if snap is None:
        return None
    return _calibrated(ticker.upper(), snap, round(rate_at(snap), 6))


__all__ = ["Smile", "smile_for", "snapshot_for"]
