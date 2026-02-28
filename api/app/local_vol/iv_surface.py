"""
Stage 3 — Implied-volatility surface.

Responsibility: take cleaned OptionQuote objects and build a smooth,
analytically differentiable IV surface in (log-moneyness, TTM) space.

Design choices
--------------
* Coordinate system: y = ln(K / F_T), T = TTM.
  - F_T = S · exp((r - q) · T) is the forward price.
  - Working in log-moneyness rather than raw strike gives a better-conditioned
    interpolation problem (quotes cluster symmetrically around y=0).
* Interpolation: 2-pass.
  1. Scattered-point linear griddata onto a regular grid of (y, T) knots.
  2. Bicubic RectBivariateSpline fit on the regular grid — gives smooth
     analytic first and second derivatives needed by the Gatheral formula.
* Black-Scholes inversion is *not* re-run here; we trust yfinance IV directly
  after the cleaning stage removed bad quotes.

Public API
----------
IVSurface           — callable object σ(K, T), with derivatives and w(K,T)
build_iv_surface(quotes, spot, rate, dividend) -> IVSurface
"""
from __future__ import annotations

import logging
from dataclasses import dataclass
from typing import List

import numpy as np
from scipy.interpolate import RectBivariateSpline, griddata
from scipy.ndimage import gaussian_filter

from .fetcher import OptionQuote

logger = logging.getLogger(__name__)

# Knot count for the intermediate regular grid
_N_Y = 80   # log-moneyness axis
_N_T = 50   # maturity axis


class IVSurface:
    """
    Smooth bicubic implied-volatility surface parameterised in
    (y, T) = (ln(K/F_T), TTM) space.

    Attributes
    ----------
    spot : float
    rate : float
    dividend : float
    y_min, y_max : float  — log-moneyness range covered by the data
    t_min, t_max : float  — TTM range covered by the data
    """

    def __init__(
        self,
        spline: RectBivariateSpline,
        spot: float,
        rate: float,
        dividend: float,
        y_min: float,
        y_max: float,
        t_min: float,
        t_max: float,
    ) -> None:
        self._spline = spline
        self.spot = spot
        self.rate = rate
        self.dividend = dividend
        self.y_min = y_min
        self.y_max = y_max
        self.t_min = t_min
        self.t_max = t_max

    # ------------------------------------------------------------------
    # Coordinate helpers
    # ------------------------------------------------------------------

    def _forward(self, T: float) -> float:
        return self.spot * np.exp((self.rate - self.dividend) * T)

    def _log_moneyness(self, K: float, T: float) -> float:
        F = self._forward(T)
        return np.log(K / F)

    def _clip(self, y: float, T: float):
        """Clamp query to calibrated domain to avoid spline extrapolation blow-up."""
        y_c = float(np.clip(y, self.y_min, self.y_max))
        T_c = float(np.clip(T, self.t_min, self.t_max))
        return y_c, T_c

    # ------------------------------------------------------------------
    # Public interface
    # ------------------------------------------------------------------

    def sigma(self, K: float, T: float) -> float:
        """Implied volatility σ(K, T)."""
        y = self._log_moneyness(K, T)
        y_c, T_c = self._clip(y, T)
        val = float(self._spline(y_c, T_c, grid=False))
        return max(val, 1e-6)  # floor to avoid negative IV artefacts

    def __call__(self, K: float, T: float) -> float:
        return self.sigma(K, T)

    def total_variance(self, K: float, T: float) -> float:
        """w(K, T) = σ²(K,T) · T  (Gatheral notation)."""
        return self.sigma(K, T) ** 2 * T

    # ------------------------------------------------------------------
    # Partial derivatives of w w.r.t. y and T  (used by Dupire stage)
    # ------------------------------------------------------------------

    def dw_dT(self, K: float, T: float, dT: float = 1e-4) -> float:
        """∂w/∂T via central finite difference."""
        T1 = max(T - dT, self.t_min)
        T2 = min(T + dT, self.t_max)
        h = T2 - T1
        return (self.total_variance(K, T2) - self.total_variance(K, T1)) / h

    def dw_dy(self, K: float, T: float) -> float:
        """∂w/∂y analytically from the bicubic spline."""
        y = self._log_moneyness(K, T)
        y_c, T_c = self._clip(y, T)
        sigma_val = self.sigma(K, T)
        dsigma_dy = float(self._spline(y_c, T_c, dx=1, grid=False))
        # w = sigma^2 * T  →  dw/dy = 2*sigma*T * dsigma/dy
        return 2.0 * sigma_val * T * dsigma_dy

    def d2w_dy2(self, K: float, T: float) -> float:
        """∂²w/∂y² analytically from the bicubic spline."""
        y = self._log_moneyness(K, T)
        y_c, T_c = self._clip(y, T)
        sigma_val = self.sigma(K, T)
        dsigma_dy  = float(self._spline(y_c, T_c, dx=1, grid=False))
        d2sigma_dy2 = float(self._spline(y_c, T_c, dx=2, grid=False))
        # d²w/dy² = 2*T*(dsigma/dy)² + 2*sigma*T*(d²sigma/dy²)
        return 2.0 * T * (dsigma_dy ** 2 + sigma_val * d2sigma_dy2)


# ---------------------------------------------------------------------------
# Builder
# ---------------------------------------------------------------------------

def build_iv_surface(
    quotes: List[OptionQuote],
    spot: float,
    rate: float,
    dividend: float,
) -> IVSurface:
    """
    Build a smooth bicubic IV surface from cleaned option quotes.

    Parameters
    ----------
    quotes : list[OptionQuote]
        Output of :func:`cleaner.clean_chain` — already filtered.
    spot : float
        Underlier spot price.
    rate : float
        Continuously compounded risk-free rate.
    dividend : float
        Continuously compounded dividend yield.

    Returns
    -------
    IVSurface
    """
    if not quotes:
        raise ValueError("build_iv_surface: no quotes provided after cleaning.")

    # --- 1. Collect (y, T, sigma) scatter points ---
    y_pts, T_pts, sig_pts = [], [], []
    for q in quotes:
        if q.implied_volatility is None or q.ttm <= 0:
            continue
        F = spot * np.exp((rate - dividend) * q.ttm)
        y = np.log(q.strike / F)
        y_pts.append(y)
        T_pts.append(q.ttm)
        sig_pts.append(q.implied_volatility)

    if len(y_pts) < 16:
        raise ValueError(
            f"build_iv_surface: only {len(y_pts)} valid IV points — "
            "too few to fit a surface.  Relax cleaning thresholds."
        )

    y_arr  = np.array(y_pts)
    T_arr  = np.array(T_pts)
    s_arr  = np.array(sig_pts)

    y_min, y_max = y_arr.min(), y_arr.max()
    t_min, t_max = T_arr.min(), T_arr.max()

    # Expand range slightly so spline can evaluate at endpoints cleanly
    dy = 0.02
    y_min -= dy; y_max += dy

    # --- 2. Griddata onto a regular (y, T) mesh ---
    y_grid = np.linspace(y_min, y_max, _N_Y)
    T_grid = np.linspace(t_min, t_max, _N_T)
    YY, TT = np.meshgrid(y_grid, T_grid, indexing="ij")  # shape (N_Y, N_T)

    sig_grid = griddata(
        points=np.column_stack([y_arr, T_arr]),
        values=s_arr,
        xi=(YY, TT),
        method="linear",
        fill_value=np.nan,
    )

    # Fill NaN boundary cells with nearest-neighbour
    nan_mask = np.isnan(sig_grid)
    if nan_mask.any():
        sig_nn = griddata(
            points=np.column_stack([y_arr, T_arr]),
            values=s_arr,
            xi=(YY, TT),
            method="nearest",
        )
        sig_grid = np.where(nan_mask, sig_nn, sig_grid)

    # Floor
    sig_grid = np.maximum(sig_grid, 0.01)

    # --- 3. Smooth the grid to kill Runge oscillations before cubic fit ---
    sig_grid = gaussian_filter(sig_grid, sigma=1.5)
    sig_grid = np.maximum(sig_grid, 0.01)  # re-floor after smoothing

    # --- 4. Bicubic spline on the regular grid ---
    spline = RectBivariateSpline(y_grid, T_grid, sig_grid, kx=3, ky=3)

    logger.info(
        "iv_surface: fitted surface on %d points | y=[%.3f,%.3f] T=[%.2f,%.2f]",
        len(y_pts), y_min, y_max, t_min, t_max,
    )

    return IVSurface(
        spline=spline,
        spot=spot,
        rate=rate,
        dividend=dividend,
        y_min=y_min,
        y_max=y_max,
        t_min=t_min,
        t_max=t_max,
    )
