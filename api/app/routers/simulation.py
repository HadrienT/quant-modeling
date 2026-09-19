"""
Router — path-simulation endpoints for the simulation page: pick a model
(Black-Scholes or SABR for now), supply parameters by hand or calibrate
against a real ticker's option chain, see simulated paths.

POST /simulation/paths/black-scholes
POST /simulation/paths/sabr
POST /simulation/calibrate

Path generation is illustrative, not a pricing engine (see
engines/mc/path_simulation.hpp's doc comments); calibration reuses the
existing DB-first market-data fetch and SVI vol-surface pipeline this
project already has (vol_surface.py, quantmodeling.calibrate_vol_surface).
"""

from __future__ import annotations

import math

import quantmodeling as qm
from fastapi import APIRouter, HTTPException

from .. import vol_surface
from ..logging_utils import get_logger
from ..schemas import (
    BSPathRequest,
    SABRPathRequest,
    SimulationCalibrateRequest,
    SimulationCalibrateResponse,
    SimulationModel,
    SimulationPathsResponse,
)
from .local_vol_pricing import _calibrate, _cleaning_summary

logger = get_logger()

router = APIRouter(prefix="/api/simulation", tags=["simulation"])


@router.post("/paths/black-scholes", response_model=SimulationPathsResponse)
def simulate_black_scholes(req: BSPathRequest) -> SimulationPathsResponse:
    result = qm.simulate_black_scholes_paths(
        spot=req.spot,
        rate=req.rate,
        dividend=req.dividend,
        vol=req.vol,
        ttm=req.ttm,
        n_steps=req.n_steps,
        n_paths=req.n_paths,
        seed=req.seed,
    )
    return SimulationPathsResponse(
        model=SimulationModel.black_scholes,
        time_grid=result["time_grid"],
        paths=result["paths"],
    )


@router.post("/paths/sabr", response_model=SimulationPathsResponse)
def simulate_sabr(req: SABRPathRequest) -> SimulationPathsResponse:
    result = qm.simulate_sabr_paths(
        forward=req.forward,
        alpha=req.alpha,
        beta=req.beta,
        rho=req.rho,
        nu=req.nu,
        ttm=req.ttm,
        n_steps=req.n_steps,
        n_paths=req.n_paths,
        seed=req.seed,
    )
    return SimulationPathsResponse(
        model=SimulationModel.sabr,
        time_grid=result["time_grid"],
        paths=result["paths"],
    )


@router.post("/calibrate", response_model=SimulationCalibrateResponse)
def calibrate(req: SimulationCalibrateRequest) -> SimulationCalibrateResponse:
    """Read the ticker's stored chain (database only), run the
    existing SVI vol-surface pipeline, pick the calibrated slice closest to
    the requested maturity, and either read its ATM vol off (Black-Scholes)
    or fit SABR to it (see vol_surface.sabr_quotes_from_svi_slice)."""
    ticker, spot, dividend, result = _calibrate(
        req.ticker,
        req.rate,
        min_open_interest=10,
        min_bid=0.05,
        max_spread_ratio=0.50,
        min_moneyness=0.70,
        max_moneyness=1.40,
    )
    stats = result["cleaning_stats"]
    if not result["slices"]:
        raise HTTPException(
            status_code=422, detail=f"No calibrated maturity slices for '{ticker}'."
        )

    slc = vol_surface.nearest_slice(result["slices"], req.ttm)
    forward = spot * math.exp((req.rate - dividend) * slc["ttm"])

    if req.model == SimulationModel.black_scholes:
        vol = vol_surface.atm_vol_from_svi_slice(slc)
        return SimulationCalibrateResponse(
            ticker=ticker,
            model=req.model,
            spot=spot,
            dividend=dividend,
            forward=forward,
            ttm=req.ttm,
            slice_ttm=slc["ttm"],
            vol=vol,
            n_clean_quotes=stats["final_count"],
            cleaning_summary=_cleaning_summary(stats),
        )

    quotes = vol_surface.sabr_quotes_from_svi_slice(slc, forward)
    if len(quotes) < 3:
        raise HTTPException(
            status_code=422,
            detail=f"Not enough usable points on the SVI slice for '{ticker}' to fit SABR.",
        )
    cal = qm.calibrate_sabr_slice(quotes=quotes, forward=forward, ttm=slc["ttm"], beta=req.beta)

    logger.info(
        "simulation calibrate: ticker=%s model=sabr slice_ttm=%.4f alpha=%.4f rho=%.4f nu=%.4f rmse=%.6f converged=%s",
        ticker,
        slc["ttm"],
        cal["alpha"],
        cal["rho"],
        cal["nu"],
        cal["rmse"],
        cal["converged"],
    )

    return SimulationCalibrateResponse(
        ticker=ticker,
        model=req.model,
        spot=spot,
        dividend=dividend,
        forward=forward,
        ttm=req.ttm,
        slice_ttm=slc["ttm"],
        alpha=cal["alpha"],
        beta=cal["beta"],
        rho=cal["rho"],
        nu=cal["nu"],
        rmse=cal["rmse"],
        converged=cal["converged"],
        n_clean_quotes=stats["final_count"],
        cleaning_summary=_cleaning_summary(stats),
    )
