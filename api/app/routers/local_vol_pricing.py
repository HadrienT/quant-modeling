"""
Router — Local-volatility pricing endpoint.

Pipeline (stage numbers match module names):
  1. Fetch raw option chain  (fetcher.py)
  2. Clean / arbitrage-filter (cleaner.py)
  3. Calibrate IV surface     (iv_surface.py)
  4. Calibrate local-vol      (dupire.py)
  5. Monte-Carlo price        (cpp_bridge.py → C++ quantmodeling)

GET  /local-vol/price
"""
from __future__ import annotations

import numpy as np
import yfinance as yf
from fastapi import APIRouter, HTTPException, Query

from ..local_vol import (
    calibrate_dupire,
    build_iv_surface,
    clean_chain,
    fetch_option_chain,
    price_with_local_vol,
)
from ..logging_utils import get_logger
from ..schemas import (
    CleanedIVSurfaceResponse,
    LocalVolRequest,
    LocalVolResponse,
    LocalVolSurfaceResponse,
)

logger = get_logger()

router = APIRouter(prefix="/local-vol", tags=["local-vol"])


def _get_spot(ticker: str) -> float:
    """Fetch the latest spot price from yfinance."""
    tick = yf.Ticker(ticker)
    try:
        price = tick.fast_info["last_price"]
        if price and price > 0:
            return float(price)
    except Exception:
        pass
    hist = tick.history(period="1d")
    if hist.empty:
        raise HTTPException(status_code=404, detail=f"No price data found for ticker '{ticker}'")
    return float(hist["Close"].iloc[-1])


def _get_dividend_yield(ticker: str) -> float:
    """Best-effort: return the trailing dividend yield as a continuous rate proxy."""
    try:
        tick = yf.Ticker(ticker)
        dy = tick.fast_info.get("dividend_yield") or 0.0
        return float(dy)
    except Exception:
        return 0.0


@router.get("/price", response_model=LocalVolResponse)
def price_local_vol(
    ticker: str = Query(..., description="Stock ticker, e.g. 'AAPL'"),
    strike: float = Query(..., gt=0, description="Option strike price"),
    maturity: float = Query(..., gt=0, description="Time to maturity in years"),
    is_call: bool = Query(True, description="True for call, False for put"),
    rate: float = Query(0.05, description="Continuously compounded risk-free rate"),
    n_paths: int = Query(50_000, ge=1000, le=500_000, description="MC simulation paths"),
    n_steps_per_year: int = Query(252, ge=12, le=2520, description="Euler steps per year"),
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
    calibrated live from the ticker's option chain.

    Steps:
    1. Fetch spot price and live option chain (yfinance).
    2. Clean quotes: liquidity, moneyness, calendar & butterfly arbitrage filters.
    3. Build smooth bicubic IV surface in log-moneyness space.
    4. Apply Gatheral (2004) formula to obtain the local-vol surface.
    5. Price via Euler-Maruyama MC with optional CRN greeks.
    """
    ticker = ticker.upper().strip()

    # --- Stage 0: spot + dividend ---
    spot    = _get_spot(ticker)
    dividend = _get_dividend_yield(ticker)

    # --- Stage 1: fetch ---
    try:
        raw_quotes = fetch_option_chain(ticker)
    except RuntimeError as exc:
        raise HTTPException(status_code=502, detail=str(exc)) from exc

    if not raw_quotes:
        raise HTTPException(
            status_code=404,
            detail=f"No option chain available for '{ticker}'.",
        )

    # --- Stage 2: clean ---
    clean_quotes, stats = clean_chain(
        raw_quotes,
        spot=spot,
        min_open_interest=min_open_interest,
        min_bid=min_bid,
        max_spread_ratio=max_spread_ratio,
        min_moneyness=min_moneyness,
        max_moneyness=max_moneyness,
    )

    if len(clean_quotes) < 16:
        raise HTTPException(
            status_code=422,
            detail=(
                f"Only {len(clean_quotes)} quotes survived cleaning (need ≥16). "
                f"Stats: {stats}. Try relaxing the cleaning thresholds."
            ),
        )

    # --- Stage 3: IV surface ---
    try:
        iv_surf = build_iv_surface(clean_quotes, spot=spot, rate=rate, dividend=dividend)
    except ValueError as exc:
        raise HTTPException(status_code=422, detail=str(exc)) from exc

    # --- Stage 4: local-vol surface ---
    try:
        lv_surf = calibrate_dupire(iv_surf, spot=spot, rate=rate, dividend=dividend)
    except RuntimeError as exc:
        raise HTTPException(status_code=500, detail=str(exc)) from exc

    # --- Stage 5: price ---
    result = price_with_local_vol(
        local_vol=lv_surf,
        spot=spot,
        strike=strike,
        maturity=maturity,
        rate=rate,
        dividend=dividend,
        is_call=is_call,
        n_paths=n_paths,
        n_steps_per_year=n_steps_per_year,
        seed=seed,
        antithetic=True,
        compute_greeks=compute_greeks,
    )

    return LocalVolResponse(
        ticker=ticker,
        spot=spot,
        npv=result.npv,
        mc_std_error=result.mc_std_error,
        delta=result.delta,
        gamma=result.gamma,
        theta=result.theta,
        rho=result.rho,
        vega_parallel=result.vega_parallel,
        n_clean_quotes=stats.final_count,
        cleaning_summary=str(stats),
        diagnostics=result.diagnostics,
    )


# -------------------------------------------------------------------------
# Shared helpers for surface-only endpoints
# -------------------------------------------------------------------------

def _fetch_and_clean(
    ticker: str,
    rate: float,
    min_open_interest: int,
    min_bid: float,
    max_spread_ratio: float,
    min_moneyness: float,
    max_moneyness: float,
):
    """Stages 0-2: spot + dividend + fetch + clean. Returns (spot, dividend, clean_quotes, stats)."""
    ticker = ticker.upper().strip()
    spot = _get_spot(ticker)
    dividend = _get_dividend_yield(ticker)
    logger.info(
        "local-vol pipeline: ticker=%s spot=%.2f dividend=%.4f rate=%.4f",
        ticker, spot, dividend, rate,
    )

    try:
        raw_quotes = fetch_option_chain(ticker)
    except RuntimeError as exc:
        raise HTTPException(status_code=502, detail=str(exc)) from exc

    if not raw_quotes:
        raise HTTPException(status_code=404, detail=f"No option chain for '{ticker}'.")

    logger.info("local-vol pipeline: fetched %d raw quotes for %s", len(raw_quotes), ticker)

    clean_quotes, stats = clean_chain(
        raw_quotes,
        spot=spot,
        min_open_interest=min_open_interest,
        min_bid=min_bid,
        max_spread_ratio=max_spread_ratio,
        min_moneyness=min_moneyness,
        max_moneyness=max_moneyness,
    )

    if len(clean_quotes) < 16:
        raise HTTPException(
            status_code=422,
            detail=f"Only {len(clean_quotes)} quotes survived cleaning (need ≥16). Stats: {stats}.",
        )

    return ticker, spot, dividend, clean_quotes, stats


def _iv_surface_to_grid(
    iv_surf,
    spot: float,
    rate: float,
    dividend: float,
    n_strikes: int = 60,
    n_maturities: int = 40,
):
    """Evaluate an IVSurface on a regular (strike, maturity) grid for JSON serialisation."""
    y_lo, y_hi = iv_surf.y_min, iv_surf.y_max
    t_lo, t_hi = iv_surf.t_min, iv_surf.t_max

    T_grid = np.linspace(t_lo, t_hi, n_maturities)
    # Convert log-moneyness bounds to strikes at mid maturity
    T_mid = 0.5 * (t_lo + t_hi)
    F_mid = spot * np.exp((rate - dividend) * T_mid)
    K_lo = F_mid * np.exp(y_lo)
    K_hi = F_mid * np.exp(y_hi)
    K_grid = np.linspace(max(K_lo, spot * 0.5), min(K_hi, spot * 2.0), n_strikes)

    values = []
    for j, T in enumerate(T_grid):
        row = []
        for i, K in enumerate(K_grid):
            try:
                iv = float(iv_surf.sigma(K, T))
                row.append(round(iv, 6) if not np.isnan(iv) else None)
            except Exception:
                row.append(None)
        values.append(row)

    return list(np.round(K_grid, 2)), list(np.round(T_grid, 4)), values


# -------------------------------------------------------------------------
# GET /local-vol/iv-surface  — cleaned & smoothed IV surface
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
    Return the cleaned & bicubic-smoothed implied-volatility surface
    (stages 0-3 of the Dupire pipeline) as a grid suitable for 3-D plotting.
    """
    ticker, spot, dividend, clean_quotes, stats = _fetch_and_clean(
        ticker, rate, min_open_interest, min_bid, max_spread_ratio,
        min_moneyness, max_moneyness,
    )

    try:
        iv_surf = build_iv_surface(clean_quotes, spot=spot, rate=rate, dividend=dividend)
    except ValueError as exc:
        raise HTTPException(status_code=422, detail=str(exc)) from exc

    strikes, maturities, values = _iv_surface_to_grid(iv_surf, spot, rate, dividend)

    return CleanedIVSurfaceResponse(
        ticker=ticker,
        spot=spot,
        strikes=strikes,
        maturities=maturities,
        values=values,
        n_clean_quotes=stats.final_count,
        cleaning_summary=str(stats),
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
    """
    Return the Dupire local-volatility surface (stages 0-4) as a grid.
    """
    ticker, spot, dividend, clean_quotes, stats = _fetch_and_clean(
        ticker, rate, min_open_interest, min_bid, max_spread_ratio,
        min_moneyness, max_moneyness,
    )

    try:
        iv_surf = build_iv_surface(clean_quotes, spot=spot, rate=rate, dividend=dividend)
    except ValueError as exc:
        raise HTTPException(status_code=422, detail=str(exc)) from exc

    try:
        lv_surf = calibrate_dupire(iv_surf, spot=spot, rate=rate, dividend=dividend)
    except RuntimeError as exc:
        raise HTTPException(status_code=500, detail=str(exc)) from exc

    # lv_surf has K_grid, T_grid, sigma_loc_flat (K-major flat array)
    n_strikes = len(lv_surf.K_grid)
    n_maturities = len(lv_surf.T_grid)
    sigma_grid = np.array(lv_surf.sigma_loc_flat).reshape(n_strikes, n_maturities)

    # Build values[maturity_idx][strike_idx] for Plotly (z[y][x])
    values = []
    for j in range(n_maturities):
        row = []
        for i in range(n_strikes):
            v = float(sigma_grid[i, j])
            row.append(round(v, 6) if not np.isnan(v) else None)
        values.append(row)

    return LocalVolSurfaceResponse(
        ticker=ticker,
        spot=spot,
        strikes=list(np.round(lv_surf.K_grid, 2)),
        maturities=list(np.round(lv_surf.T_grid, 4)),
        values=values,
        n_clean_quotes=stats.final_count,
        cleaning_summary=str(stats),
    )
