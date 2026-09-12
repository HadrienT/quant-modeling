"""
Router — Local-volatility pricing endpoints.

Pipeline:
  1. Fetch raw option chain   (vol_surface.py: stored snapshot, else live yfinance)
  2. Clean + calibrate SVI    (C++: quantmodeling.calibrate_vol_surface)
  3. Dupire local-vol grid    (C++: same call)
  4. Monte-Carlo price        (C++: quantmodeling.price_local_vol_mc)

GET  /local-vol/price
GET  /local-vol/iv-surface
GET  /local-vol/surface
"""

from __future__ import annotations

import quantmodeling as qm
from fastapi import APIRouter, HTTPException, Query

from .. import vol_surface
from ..logging_utils import get_logger
from ..schemas import (
    CleanedIVSurfaceResponse,
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


def _calibrate(
    ticker: str,
    rate: float,
    min_open_interest: int,
    min_bid: float,
    max_spread_ratio: float,
    min_moneyness: float,
    max_moneyness: float,
    k_min: float = -0.6,
    k_max: float = 0.6,
    n_strikes: int = 100,
    n_maturities: int = 50,
) -> tuple:
    """Fetch + calibrate. Returns (ticker, spot, dividend, result_dict)."""
    ticker = ticker.upper().strip()
    spot = vol_surface.get_spot(ticker)
    dividend = vol_surface.get_dividend_yield(ticker)

    try:
        raw_quotes = vol_surface.fetch_option_chain(ticker)
    except RuntimeError as exc:
        raise HTTPException(status_code=502, detail=str(exc)) from exc

    if not raw_quotes:
        raise HTTPException(
            status_code=404, detail=f"No option chain available for '{ticker}'."
        )

    logger.info(
        "local-vol: ticker=%s spot=%.2f dividend=%.4f rate=%.4f n_raw=%d",
        ticker,
        spot,
        dividend,
        rate,
        len(raw_quotes),
    )

    cleaning_params = qm.CleaningParams()
    cleaning_params.min_open_interest = min_open_interest
    cleaning_params.min_bid = min_bid
    cleaning_params.max_spread_ratio = max_spread_ratio
    cleaning_params.min_moneyness = min_moneyness
    cleaning_params.max_moneyness = max_moneyness

    try:
        result = qm.calibrate_vol_surface(
            raw_quotes,
            spot,
            rate,
            dividend,
            k_min,
            k_max,
            n_strikes,
            n_maturities,
            cleaning_params=cleaning_params,
        )
    except RuntimeError as exc:
        raise HTTPException(status_code=422, detail=str(exc)) from exc

    return ticker, spot, dividend, result


@router.get("/price", response_model=LocalVolResponse)
def price_local_vol(
    ticker: str = Query(..., description="Stock ticker, e.g. 'AAPL'"),
    strike: float = Query(..., gt=0, description="Option strike price"),
    maturity: float = Query(..., gt=0, description="Time to maturity in years"),
    is_call: bool = Query(True, description="True for call, False for put"),
    rate: float = Query(0.05, description="Continuously compounded risk-free rate"),
    n_paths: int = Query(
        50_000, ge=1000, le=500_000, description="MC simulation paths"
    ),
    n_steps_per_year: int = Query(
        252, ge=12, le=2520, description="Euler steps per year"
    ),
    seed: int = Query(1, description="RNG seed"),
    compute_greeks: bool = Query(True, description="Compute finite-difference greeks"),
    min_open_interest: int = Query(10, ge=1),
    min_bid: float = Query(0.05, ge=0.0),
    max_spread_ratio: float = Query(0.50, gt=0.0, le=1.0),
    min_moneyness: float = Query(0.70, gt=0.0),
    max_moneyness: float = Query(1.40, gt=0.0),
) -> LocalVolResponse:
    """
    Price a European vanilla option using a Dupire local-volatility surface
    calibrated from the ticker's option chain: clean -> SVI per maturity ->
    Dupire (Gatheral closed form) -> Euler-Maruyama Monte Carlo.
    """
    ticker, spot, dividend, result = _calibrate(
        ticker,
        rate,
        min_open_interest,
        min_bid,
        max_spread_ratio,
        min_moneyness,
        max_moneyness,
    )

    mc_input = qm.LocalVolInput()
    mc_input.spot = spot
    mc_input.strike = strike
    mc_input.maturity = maturity
    mc_input.rate = rate
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
# GET /local-vol/iv-surface  — SVI-fitted, arbitrage-checked IV surface
# -------------------------------------------------------------------------


@router.get("/iv-surface", response_model=CleanedIVSurfaceResponse)
def cleaned_iv_surface(
    ticker: str = Query(..., description="Stock ticker"),
    rate: float = Query(0.05, description="Risk-free rate"),
    min_open_interest: int = Query(10, ge=1),
    min_bid: float = Query(0.05, ge=0.0),
    max_spread_ratio: float = Query(0.50, gt=0.0, le=1.0),
    min_moneyness: float = Query(0.70, gt=0.0),
    max_moneyness: float = Query(1.40, gt=0.0),
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
    ticker, spot, dividend, result = _calibrate(
        ticker,
        rate,
        min_open_interest,
        min_bid,
        max_spread_ratio,
        min_moneyness,
        max_moneyness,
    )
    stats = result["cleaning_stats"]

    # Reuse the exact log-moneyness range calibrate_vol_surface actually used
    # for the Dupire grid (already clamped to what every slice observed --
    # see VolSurfacePipelineResult::k_min), so this surface and /surface
    # never disagree about how far to extrapolate.
    strikes, maturities, values = vol_surface.svi_implied_vol_grid(
        result["slices"],
        spot,
        rate,
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
    rate: float = Query(0.05, description="Risk-free rate"),
    min_open_interest: int = Query(10, ge=1),
    min_bid: float = Query(0.05, ge=0.0),
    max_spread_ratio: float = Query(0.50, gt=0.0, le=1.0),
    min_moneyness: float = Query(0.70, gt=0.0),
    max_moneyness: float = Query(1.40, gt=0.0),
) -> LocalVolSurfaceResponse:
    """The Dupire local-volatility surface, computed analytically from the
    calibrated SVI slices (market/dupire_from_svi.hpp) -- no finite
    differences anywhere in the chain from raw quotes to this grid."""
    ticker, spot, dividend, result = _calibrate(
        ticker,
        rate,
        min_open_interest,
        min_bid,
        max_spread_ratio,
        min_moneyness,
        max_moneyness,
    )
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
