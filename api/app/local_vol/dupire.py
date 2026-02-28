"""
Stage 4 — Dupire local-volatility calibration.

Responsibility: given an IVSurface object, compute the local-vol grid
σ_loc(K, T) using the Gatheral (2004) numerically-stable form of Dupire's
equation, then wrap it in a 2-D interpolator.

Gatheral formula
----------------
Let  w(y,T) = σ²_impl(K,T) · T   (total implied variance)
     y      = ln(K / F_T)         (log-moneyness)

        σ²_loc(K,T)  =
              w'_T
    ─────────────────────────────────────────────────────────────────
    1  −  (y/w)·w'_y  +  ¼(−¼ − 1/w + y²/w²)·(w'_y)²  +  ½·w''_yy

All partials are taken from the smooth bicubic spline of the IV surface,
so we only evaluate finite differences for ∂w/∂T (which only needs Δt).

Stability guards
----------------
* Denominator clamped away from 0 (floor 0.05) to avoid blow-up.
* σ²_loc clamped to [ε, 4.0] (i.e. σ_loc in [~1%, 200%]).
* The grid is only evaluated where the IV surface is well-covered.

Public API
----------
LocalVolSurface     — callable σ_loc(S, t); S=spot-level, t=calendar time
calibrate_dupire(iv_surface, spot, rate, dividend, ...) -> LocalVolSurface
"""
from __future__ import annotations

import logging

import numpy as np
from scipy.interpolate import RectBivariateSpline
from scipy.ndimage import gaussian_filter

from .iv_surface import IVSurface

logger = logging.getLogger(__name__)

_DENOM_FLOOR = 0.05      # absolute floor for the Gatheral denominator
_LOC_VAR_MIN = (0.01)**2  # 1% local vol  → 0.0001
_LOC_VAR_MAX = (4.00)**2  # 400% local vol → 16   (effectively uncapped)


class LocalVolSurface:
    """
    2-D interpolator for σ_loc(S, t) calibrated via Dupire/Gatheral.

    Call signature:  sigma_loc = lv(S, t)

    Attributes
    ----------
    spot  : float   — underlier spot at calibration time
    t_min : float   — shortest maturity on the grid
    t_max : float   — longest  maturity on the grid
    K_grid        : list[float]  — strike axis of the backing grid
    T_grid        : list[float]  — maturity axis of the backing grid
    sigma_loc_flat: list[float]  — σ_loc values, row-major (K-major):
                                   sigma_loc_flat[i*len(T_grid)+j] = σ_loc(K_grid[i], T_grid[j])
    """

    def __init__(
        self,
        spline: RectBivariateSpline,
        spot: float,
        rate: float,
        dividend: float,
        S_min: float,
        S_max: float,
        t_min: float,
        t_max: float,
        K_grid: np.ndarray,
        T_grid: np.ndarray,
        sigma_loc_flat: np.ndarray,
    ) -> None:
        self._spline = spline
        self.spot = spot
        self.rate = rate
        self.dividend = dividend
        self.S_min = S_min
        self.S_max = S_max
        self.t_min = t_min
        self.t_max = t_max
        # Grid exposed to the C++ MC engine
        self.K_grid: list[float] = K_grid.tolist()
        self.T_grid: list[float] = T_grid.tolist()
        self.sigma_loc_flat: list[float] = sigma_loc_flat.tolist()

    def __call__(self, S: float, t: float) -> float:
        """σ_loc(S, t)."""
        S_c = float(np.clip(S, self.S_min, self.S_max))
        t_c = float(np.clip(t, self.t_min, self.t_max))
        sigma = float(self._spline(S_c, t_c))
        sigma = float(np.clip(sigma, np.sqrt(_LOC_VAR_MIN), np.sqrt(_LOC_VAR_MAX)))
        return sigma


# ---------------------------------------------------------------------------
# Calibration
# ---------------------------------------------------------------------------

def _gatheral_local_var(
    iv_surf: IVSurface,
    K: float,
    T: float,
) -> float:
    """
    Compute σ²_loc(K, T) via the Gatheral formula.

    Returns NaN if evaluation is unstable (denominator too small,
    total variance too small, etc.).
    """
    w = iv_surf.total_variance(K, T)
    if w < 1e-8:
        return float("nan")

    y = iv_surf._log_moneyness(K, T)    # noqa: SLF001

    dw_dT  = iv_surf.dw_dT(K, T)
    dw_dy  = iv_surf.dw_dy(K, T)
    d2w_dy2 = iv_surf.d2w_dy2(K, T)

    if dw_dT < 0:
        # Calendar arbitrage: force to a small positive number rather than NaN
        dw_dT = max(dw_dT, 1e-6)

    denom = (
        1.0
        - (y / w) * dw_dy
        + 0.25 * (-0.25 - 1.0 / w + y**2 / w**2) * dw_dy**2
        + 0.5 * d2w_dy2
    )

    if abs(denom) < _DENOM_FLOOR:
        return float("nan")

    local_var = dw_dT / denom
    if local_var < 0:
        return float("nan")

    return local_var


def calibrate_dupire(
    iv_surface: IVSurface,
    spot: float,
    rate: float,
    dividend: float,
    n_strikes: int = 100,
    n_maturities: int = 50,
) -> LocalVolSurface:
    """
    Build a :class:`LocalVolSurface` from an :class:`~iv_surface.IVSurface`.

    Parameters
    ----------
    iv_surface : IVSurface
        Calibrated BS IV surface (output of :func:`iv_surface.build_iv_surface`).
    spot : float
        Underlier spot price.
    rate : float
        Continuously compounded risk-free rate.
    dividend : float
        Continuously compounded dividend yield.
    n_strikes : int
        Number of strike grid points (default 100).
    n_maturities : int
        Number of maturity grid points (default 50).

    Returns
    -------
    LocalVolSurface
    """
    # Build a uniform grid in (S, T) space covering the IV-surface domain
    y_lo, y_hi = iv_surface.y_min, iv_surface.y_max
    t_lo, t_hi = iv_surface.t_min, iv_surface.t_max

    T_grid = np.linspace(t_lo, t_hi, n_maturities)
    # Convert log-moneyness extremes to strike extremes at midpoint maturity
    T_mid = 0.5 * (t_lo + t_hi)
    F_mid = spot * np.exp((rate - dividend) * T_mid)
    K_lo  = F_mid * np.exp(y_lo)
    K_hi  = F_mid * np.exp(y_hi)
    K_grid = np.linspace(max(K_lo, spot * 0.2), min(K_hi, spot * 3.0), n_strikes)

    loc_var_grid = np.zeros((n_strikes, n_maturities))

    nan_count = 0
    for i, K in enumerate(K_grid):
        for j, T in enumerate(T_grid):
            lv = _gatheral_local_var(iv_surface, K, T)
            if np.isnan(lv):
                nan_count += 1
                lv = float(np.nan)
            loc_var_grid[i, j] = lv

    # --- Fill NaN cells with nearest-neighbour from the valid grid ---
    from scipy.interpolate import griddata as _griddata
    KK, TT = np.meshgrid(K_grid, T_grid, indexing="ij")
    valid = ~np.isnan(loc_var_grid)
    if valid.sum() < 4:
        raise RuntimeError(
            "calibrate_dupire: fewer than 4 valid local-vol cells — "
            "IV surface may be too sparse or heavily extrapolated."
        )

    if not valid.all():
        filled = _griddata(
            points=np.column_stack([KK[valid], TT[valid]]),
            values=loc_var_grid[valid],
            xi=np.column_stack([KK.ravel(), TT.ravel()]),
            method="nearest",
        ).reshape(n_strikes, n_maturities)
        loc_var_grid = np.where(valid, loc_var_grid, filled)

    # Clamp
    loc_var_grid = np.clip(loc_var_grid, _LOC_VAR_MIN, _LOC_VAR_MAX)

    # σ_loc grid (square root of local variance) — passed to C++ engine
    sigma_loc_grid = np.sqrt(loc_var_grid)   # shape (n_strikes, n_maturities)

    # Smooth to remove spiky artifacts from derivative amplification
    sigma_loc_grid = gaussian_filter(sigma_loc_grid, sigma=2.0)
    sigma_loc_grid = np.clip(sigma_loc_grid, np.sqrt(_LOC_VAR_MIN), np.sqrt(_LOC_VAR_MAX))

    sigma_loc_flat_np = sigma_loc_grid.ravel()  # row-major (K-major)

    # Bicubic spline on (S, T) grid
    spline = RectBivariateSpline(K_grid, T_grid, sigma_loc_grid, kx=3, ky=3)

    logger.info(
        "dupire: calibrated local-vol surface | %d×%d grid | %d NaN filled",
        n_strikes, n_maturities, nan_count,
    )

    return LocalVolSurface(
        spline=spline,
        spot=spot,
        rate=rate,
        dividend=dividend,
        S_min=K_grid[0],
        S_max=K_grid[-1],
        t_min=t_lo,
        t_max=t_hi,
        K_grid=K_grid,
        T_grid=T_grid,
        sigma_loc_flat=sigma_loc_flat_np,
    )
