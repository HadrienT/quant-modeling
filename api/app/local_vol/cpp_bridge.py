"""
Stage 5 — C++ engine bridge (cpp_bridge.py).

Responsibility: thin Python adapter that bridges the calibrated
:class:`~dupire.LocalVolSurface` (Python) to the C++ Euler-Maruyama MC engine
(``quantmodeling.price_local_vol_mc``).

The C++ engine handles:
  - Log-Euler path simulation with antithetic variates
  - CRN finite-difference greeks (Δ, Γ, Θ, ρ, vega parallel)
  - Welford online variance for standard errors

This module has no MC logic of its own; it only packs grid data into a
``LocalVolInput`` struct and unpacks the ``PricingResult`` dict.

Public API
----------
LocalVolPricingResult
price_with_local_vol(local_vol, spot, strike, ...) -> LocalVolPricingResult
"""
from __future__ import annotations

import logging
from dataclasses import dataclass, field
from typing import Optional

from .dupire import LocalVolSurface

logger = logging.getLogger(__name__)


# ---------------------------------------------------------------------------
# Lazy import of the C++ module
# ---------------------------------------------------------------------------
def _import_cpp():
    try:
        import quantmodeling  # type: ignore[import]
        return quantmodeling
    except ImportError as exc:
        raise RuntimeError(
            "The C++ extension 'quantmodeling' is not available.  "
            "Build it with: cmake --build build --target quantmodeling  "
            "(requires QM_BUILD_PYTHON=ON)."
        ) from exc


# ---------------------------------------------------------------------------
# Public result dataclass
# ---------------------------------------------------------------------------

@dataclass
class LocalVolPricingResult:
    """Pricing result + greeks from the C++ local-vol MC engine."""
    npv: float
    mc_std_error: float
    delta: Optional[float] = None
    gamma: Optional[float] = None
    theta: Optional[float] = None
    rho: Optional[float] = None
    vega_parallel: Optional[float] = None    # dP/d(+1% flat σ_loc shift)
    diagnostics: str = field(default="")


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

def price_with_local_vol(
    local_vol: LocalVolSurface,
    spot: float,
    strike: float,
    maturity: float,
    rate: float,
    dividend: float,
    is_call: bool,
    n_paths: int = 50_000,
    n_steps_per_year: int = 252,
    seed: int = 1,
    antithetic: bool = True,
    compute_greeks: bool = True,
) -> LocalVolPricingResult:
    """
    Price a European vanilla option under a local-vol surface using the C++
    Euler-Maruyama MC engine.

    Parameters
    ----------
    local_vol : LocalVolSurface
        Calibrated Dupire surface from :func:`dupire.calibrate_dupire`.
        Must expose ``K_grid``, ``T_grid``, and ``sigma_loc_flat``.
    spot, strike, maturity, rate, dividend : float
    is_call : bool
    n_paths : int
        MC simulation paths (default 50 000).
    n_steps_per_year : int
        Euler time-steps per year (default 252).
    seed : int
    antithetic : bool
        Antithetic variates (default True).
    compute_greeks : bool
        If False, skip greek computation (faster for quote-mode).

    Returns
    -------
    LocalVolPricingResult
    """
    if maturity <= 0:
        raise ValueError("maturity must be > 0")

    qm = _import_cpp()

    # --- Pack grid into C++ input struct ---
    inp = qm.LocalVolInput()
    inp.spot             = float(spot)
    inp.strike           = float(strike)
    inp.maturity         = float(maturity)
    inp.rate             = float(rate)
    inp.dividend         = float(dividend)
    inp.is_call          = bool(is_call)
    inp.K_grid           = local_vol.K_grid          # list[float]
    inp.T_grid           = local_vol.T_grid          # list[float]
    inp.sigma_loc_flat   = local_vol.sigma_loc_flat  # list[float], K-major
    inp.n_paths          = int(n_paths)
    inp.n_steps_per_year = int(n_steps_per_year)
    inp.seed             = int(seed)
    inp.mc_antithetic    = bool(antithetic)
    inp.compute_greeks   = bool(compute_greeks)

    # --- Call C++ engine ---
    result_dict: dict = qm.price_local_vol_mc(inp)

    greeks = result_dict.get("greeks", {})

    res = LocalVolPricingResult(
        npv=float(result_dict["npv"]),
        mc_std_error=float(result_dict["mc_std_error"]),
        diagnostics=str(result_dict.get("diagnostics", "")),
        delta=greeks.get("delta"),
        gamma=greeks.get("gamma"),
        theta=greeks.get("theta"),
        rho=greeks.get("rho"),
        vega_parallel=greeks.get("vega"),  # C++ key is "vega" (parallel shift)
    )

    logger.info(
        "cpp_bridge (C++): NPV=%.4f SE=%.4f Δ=%s Γ=%s Θ=%s ρ=%s vega=%s",
        res.npv, res.mc_std_error,
        f"{res.delta:.4f}" if res.delta is not None else "—",
        f"{res.gamma:.6f}" if res.gamma is not None else "—",
        f"{res.theta:.4f}" if res.theta is not None else "—",
        f"{res.rho:.4f}" if res.rho is not None else "—",
        f"{res.vega_parallel:.4f}" if res.vega_parallel is not None else "—",
    )

    return res
