"""
Router — Local-volatility pricing endpoints.

Pipeline:
  1. Stored option chain      (market_snapshot: the database only, QM_OPTION_DATA)
  2. Clean + calibrate SVI    (C++: quantmodeling.calibrate_vol_surface)
  3. Dupire local-vol grid    (C++: same call)
  4. Monte-Carlo price        (C++: quantmodeling.price_local_vol_mc)

GET  /local-vol/price
GET  /local-vol/iv-surface
GET  /local-vol/surface
GET  /local-vol/raw-surface
GET  /local-vol/delta-surface
"""

from __future__ import annotations

from datetime import date
from typing import Optional

import quantmodeling as qm
from fastapi import APIRouter, HTTPException, Query

from .. import market_snapshot, vol_surface
from ..logging_utils import get_logger
from ..schemas import (
    CleanedIVSurfaceResponse,
    DeltaBucketRow,
    DeltaSurfaceResponse,
    LocalVolResponse,
    LocalVolSurfaceResponse,
)

logger = get_logger()

router = APIRouter(prefix="/api/local-vol", tags=["local-vol"])


def _cleaning_summary(stats: dict) -> str:
    return (
        f"raw={stats['raw_count']} → "
        f"liquidity={stats['after_liquidity']} → "
        f"moneyness={stats['after_moneyness']} → "
        f"calendar_arb={stats['after_calendar_arbitrage']} → "
        f"butterfly_arb={stats['after_butterfly_arbitrage']}"
    )


#: The rate the local-vol routes used when none is given, kept under legacy.
LEGACY_DEFAULT_RATE = 0.05


def _calibrate(ticker: str, rate: Optional[float]) -> tuple:
    """The stored surface of `ticker` (market_snapshot: the database only,
    no live source; the option data QM_OPTION_DATA names). Returns (ticker,
    spot, dividend, result) with result shaped as calibrate_vol_surface's."""
    ticker = ticker.upper().strip()
    if rate is None and market_snapshot.option_data() == "legacy":
        rate = LEGACY_DEFAULT_RATE
    try:
        m = market_snapshot.local_vol_market(ticker, rate, date.today())
    except market_snapshot.MarketDataUnavailable as exc:
        raise HTTPException(status_code=422, detail=str(exc)) from exc
    except RuntimeError as exc:  # the native calibration refused the chain
        raise HTTPException(status_code=422, detail=str(exc)) from exc
    result = {
        "slices": m.svi_slices,
        "K_grid": m.K_grid,
        "T_grid": m.T_grid,
        "sigma_loc_flat": m.sigma_loc_flat,
        "k_min": m.k_min,
        "k_max": m.k_max,
        "cleaning_stats": m.cleaning_stats,
        "rate": m.rate,  # the rate given, or the market's (chain_iv)
    }
    return ticker, m.spot, m.dividend, result


@router.get("/price", response_model=LocalVolResponse)
def price_local_vol(
    ticker: str = Query(..., description="Stock ticker, e.g. 'AAPL'"),
    strike: float = Query(..., gt=0, description="Option strike price"),
    maturity: float = Query(..., gt=0, description="Time to maturity in years"),
    is_call: bool = Query(True, description="True for call, False for put"),
    rate: Optional[float] = Query(
        None,
        description="Override of the market's discount rate (continuously "
        "compounded). Empty: the market's (QM_OPTION_DATA=chain_iv), or 5 % under legacy.",
    ),
    n_paths: int = Query(
        50_000, ge=1000, le=500_000, description="MC simulation paths"
    ),
    n_steps_per_year: int = Query(
        252, ge=12, le=2520, description="Euler steps per year"
    ),
    seed: int = Query(1, description="RNG seed"),
    compute_greeks: bool = Query(True, description="Compute finite-difference greeks"),
) -> LocalVolResponse:
    """
    Price a European vanilla option using a Dupire local-volatility surface
    calibrated from the ticker's option chain: clean -> SVI per maturity ->
    Dupire (Gatheral closed form) -> Euler-Maruyama Monte Carlo.
    """
    ticker, spot, dividend, result = _calibrate(ticker, rate)

    mc_input = qm.LocalVolInput()
    mc_input.spot = spot
    mc_input.strike = strike
    mc_input.maturity = maturity
    mc_input.rate = result["rate"]
    mc_input.dividend = dividend
    mc_input.is_call = is_call
    mc_input.K_grid = result["K_grid"]
    mc_input.T_grid = result["T_grid"]
    mc_input.sigma_loc_flat = result["sigma_loc_flat"]
    mc_input.n_paths = n_paths
    mc_input.n_steps_per_year = n_steps_per_year
    mc_input.seed = seed
    mc_input.mc_antithetic = True
    mc_input.compute_greeks = compute_greeks

    priced = qm.price_local_vol_mc(mc_input)
    greeks = priced["greeks"]
    stats = result["cleaning_stats"]

    return LocalVolResponse(
        ticker=ticker,
        spot=spot,
        npv=priced["npv"],
        mc_std_error=priced["mc_std_error"],
        delta=greeks.get("delta"),
        gamma=greeks.get("gamma"),
        theta=greeks.get("theta"),
        rho=greeks.get("rho"),
        vega_parallel=greeks.get("vega"),
        n_clean_quotes=stats["final_count"],
        cleaning_summary=_cleaning_summary(stats),
        diagnostics=priced["diagnostics"],
    )


# -------------------------------------------------------------------------
# GET /local-vol/raw-surface  — the actual listed grid, no fitting, no
# interpolation (replaces the old /market/iv/surface griddata path, which
# manufactured a fully-populated surface via linear interpolation +
# nearest-neighbour fill -- defeating the point of a "raw" view before
# cleaning even runs).
# -------------------------------------------------------------------------


@router.get("/raw-surface", response_model=CleanedIVSurfaceResponse)
def raw_iv_surface(
    ticker: str = Query(..., description="Stock ticker"),
) -> CleanedIVSurfaceResponse:
    """The listed option chain's implied vols, gridded on the strikes and
    maturities actually traded -- no cleaning, no fitting, no
    interpolation. See vol_surface.raw_iv_grid."""
    try:
        raw_quotes, spot, _ = market_snapshot.stored_chain(ticker, date.today())
    except market_snapshot.MarketDataUnavailable as exc:
        raise HTTPException(status_code=422, detail=str(exc)) from exc
    ticker = ticker.upper().strip()
    if not raw_quotes:
        raise HTTPException(
            status_code=404, detail=f"No stored option chain for '{ticker}'."
        )

    strikes, maturities, values = vol_surface.raw_iv_grid(raw_quotes)
    n_with_iv = sum(1 for q in raw_quotes if q.has_iv)

    return CleanedIVSurfaceResponse(
        ticker=ticker,
        spot=spot,
        strikes=[round(k, 2) for k in strikes],
        maturities=[round(t, 4) for t in maturities],
        values=values,
        n_clean_quotes=n_with_iv,
        cleaning_summary=(
            f"raw quotes: {len(raw_quotes)} -> with a usable implied vol: {n_with_iv}. "
            "No cleaning applied -- see 'Cleaned' for the arbitrage-checked surface."
        ),
    )


# -------------------------------------------------------------------------
# GET /local-vol/delta-surface  — the desk view: a delta-bucketed vol matrix
# (10/25-delta put, ATM, 25/10-delta call) per maturity, plus risk reversals
# and butterflies. This is what an options desk actually looks at day to
# day -- a dense numeric grid quoted in delta, not a strike-indexed surface
# plot (see the "Volatility" tab's own docstring on the front end for the
# fuller rationale). Skew is RR25 = vol(25c) - vol(25p); convexity is
# BF25 = 0.5*(vol(25c)+vol(25p)) - vol(ATM) -- literally how FX vol desks
# quote the smile, rather than strike-by-strike implied vols.
# -------------------------------------------------------------------------


@router.get("/delta-surface", response_model=DeltaSurfaceResponse)
def delta_surface(
    ticker: str = Query(..., description="Stock ticker"),
    rate: Optional[float] = Query(
        None,
        description="Override of the market's discount rate. Empty: the market's "
        "(QM_OPTION_DATA=chain_iv), or 5 % under legacy.",
    ),
) -> DeltaSurfaceResponse:
    """Every calibrated SVI slice, reduced to its desk-standard delta
    buckets (vol_surface.delta_bucket_row) -- reuses the same calibration
    /local-vol/iv-surface and /local-vol/surface already run, just a
    different, more desk-familiar reduction of it."""
    ticker, spot, dividend, result = _calibrate(ticker, rate)
    stats = result["cleaning_stats"]

    rows = [
        DeltaBucketRow(**vol_surface.delta_bucket_row(slc))
        for slc in sorted(result["slices"], key=lambda s: s["ttm"])
    ]

    return DeltaSurfaceResponse(
        ticker=ticker,
        spot=spot,
        rows=rows,
        n_clean_quotes=stats["final_count"],
        cleaning_summary=_cleaning_summary(stats),
    )


# -------------------------------------------------------------------------
# GET /local-vol/iv-surface  — SVI-fitted, arbitrage-checked IV surface
# -------------------------------------------------------------------------


@router.get("/iv-surface", response_model=CleanedIVSurfaceResponse)
def cleaned_iv_surface(
    ticker: str = Query(..., description="Stock ticker"),
    rate: Optional[float] = Query(
        None,
        description="Override of the market's discount rate. Empty: the market's "
        "(QM_OPTION_DATA=chain_iv), or 5 % under legacy.",
    ),
) -> CleanedIVSurfaceResponse:
    """
    The SVI-fitted implied-volatility surface, one calibrated slice per
    maturity with enough clean quotes, linearly interpolated in total
    variance between them -- replaces the old bicubic-spline-plus-Gaussian-
    blur surface with one whose butterfly/calendar arbitrage status is
    actually checked (see each slice's `converged` / arbitrage flags below,
    surfaced once the front-end calibration-report screen exists per
    blueprint/wp/15-future-quant-surfaces.md §1).
    """
    ticker, spot, dividend, result = _calibrate(ticker, rate)
    stats = result["cleaning_stats"]

    # Reuse the exact log-moneyness range calibrate_vol_surface actually used
    # for the Dupire grid (already clamped to what every surface slice
    # observed -- see VolSurfacePipelineResult::k_min), so this surface and /surface
    # never disagree about how far to extrapolate.
    strikes, maturities, values = vol_surface.svi_implied_vol_grid(
        result["slices"],
        spot,
        result["rate"],
        dividend,
        k_min=result["k_min"],
        k_max=result["k_max"],
    )

    return CleanedIVSurfaceResponse(
        ticker=ticker,
        spot=spot,
        strikes=[round(k, 2) for k in strikes],
        maturities=[round(t, 4) for t in maturities],
        values=values,
        n_clean_quotes=stats["final_count"],
        cleaning_summary=_cleaning_summary(stats),
    )


# -------------------------------------------------------------------------
# GET /local-vol/surface  — Dupire local-vol surface
# -------------------------------------------------------------------------


@router.get("/surface", response_model=LocalVolSurfaceResponse)
def local_vol_surface(
    ticker: str = Query(..., description="Stock ticker"),
    rate: Optional[float] = Query(
        None,
        description="Override of the market's discount rate. Empty: the market's "
        "(QM_OPTION_DATA=chain_iv), or 5 % under legacy.",
    ),
) -> LocalVolSurfaceResponse:
    """The Dupire local-volatility surface, computed analytically from the
    calibrated SVI slices (market/dupire_from_svi.hpp) -- no finite
    differences anywhere in the chain from raw quotes to this grid."""
    ticker, spot, dividend, result = _calibrate(ticker, rate)
    stats = result["cleaning_stats"]

    K_grid = result["K_grid"]
    T_grid = result["T_grid"]
    n_strikes, n_maturities = len(K_grid), len(T_grid)
    sigma_flat = result["sigma_loc_flat"]  # K-major: sigma_flat[i*n_maturities+j]

    values = [
        [round(sigma_flat[i * n_maturities + j], 6) for i in range(n_strikes)]
        for j in range(n_maturities)
    ]

    return LocalVolSurfaceResponse(
        ticker=ticker,
        spot=spot,
        strikes=[round(k, 2) for k in K_grid],
        maturities=[round(t, 4) for t in T_grid],
        values=values,
        n_clean_quotes=stats["final_count"],
        cleaning_summary=_cleaning_summary(stats),
    )
