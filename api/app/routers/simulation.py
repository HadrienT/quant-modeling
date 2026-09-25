"""
Router — path-simulation endpoints for the simulation page: pick a model
(Black-Scholes, SABR, local vol, Heston, SLV), supply parameters by hand or calibrate
against a real ticker's option chain, see simulated paths.

POST /simulation/paths/black-scholes
POST /simulation/paths/sabr
POST /simulation/paths/model        local vol, Heston, SLV
POST /simulation/calibrate

Path generation is illustrative, not a pricing engine (see
engines/mc/path_simulation.hpp's doc comments); calibration reuses the
existing DB-first market-data fetch and SVI vol-surface pipeline this
project already has (vol_surface.py, quantmodeling.calibrate_vol_surface).
"""

from __future__ import annotations

import math
from datetime import date

import quantmodeling as qm
from fastapi import APIRouter, HTTPException

from .. import market_snapshot, stochastic_vol, vol_surface
from ..logging_utils import get_logger
from ..schemas import (
    BSPathRequest,
    HestonFit,
    LeverageFit,
    ModelPathRequest,
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


def _market(ticker: str, rate: float):
    """The stored surface of `ticker` (database only, never a live source)."""
    try:
        return market_snapshot.local_vol_market(ticker, rate, date.today())
    except market_snapshot.MarketDataUnavailable as exc:
        raise HTTPException(status_code=422, detail=str(exc)) from exc


def _stochastic(market):
    try:
        return stochastic_vol.calibrate(market)
    except stochastic_vol.CalibrationUnavailable as exc:
        raise HTTPException(status_code=422, detail=str(exc)) from exc


def _calibrate_surface_model(
    req: SimulationCalibrateRequest,
) -> SimulationCalibrateResponse:
    """Local vol, Heston, SLV: calibrated on the stored option chain only
    (market_snapshot + stochastic_vol, as the scripting page does)."""
    m = _market(req.ticker, req.rate)
    out = dict(
        ticker=m.ticker,
        model=req.model,
        spot=m.spot,
        dividend=m.dividend,
        forward=m.spot * math.exp((req.rate - m.dividend) * req.ttm),
        ttm=req.ttm,
        snapshot=m.valuation_date,
        surface=(
            f"{len(m.svi_slices)} SVI slices, Dupire grid "
            f"{len(m.K_grid)} strikes x {len(m.T_grid)} maturities up to "
            f"{m.T_grid[-1]:.2f}y"
        ),
    )
    if req.model != SimulationModel.local_vol:
        sv = _stochastic(m)
        out["heston"] = HestonFit(
            **sv.heston,
            iv_rmse=sv.iv_rmse,
            iv_worst=sv.iv_worst,
            n_quotes=sv.n_quotes,
            n_maturities=sv.n_maturities,
            feller=sv.feller,
        )
        if req.model == SimulationModel.slv:
            out["leverage"] = LeverageFit(
                min=sv.leverage_min,
                max=sv.leverage_max,
                clamped_share=sv.leverage_clamped_share,
                n_particles=sv.n_particles,
            )
    return SimulationCalibrateResponse(**out)


@router.post("/paths/model", response_model=SimulationPathsResponse)
def simulate_model(req: ModelPathRequest) -> SimulationPathsResponse:
    """Local vol, Heston or SLV paths (engines/mc/path_simulation.hpp's
    simulate_model_paths, the same models the script pricer uses)."""
    kw: dict = dict(
        ttm=req.ttm, n_steps=req.n_steps, n_paths=req.n_paths, seed=req.seed
    )
    if req.model == "heston" and req.heston is not None and req.spot is not None:
        spot, dividend, heston = req.spot, req.dividend or 0.0, req.heston.model_dump()
        kw.update(heston=heston)
    else:
        m = _market(req.ticker, req.rate)
        spot, dividend = m.spot, m.dividend
        if req.model == "local_vol":
            kw.update(K_grid=m.K_grid, T_grid=m.T_grid, sigma_loc_flat=m.sigma_loc_flat)
        else:
            sv = _stochastic(m)
            kw.update(heston=req.heston.model_dump() if req.heston else sv.heston)
            if req.model == "slv":
                # The leverage was calibrated with the calibrated Heston
                # parameters: SLV always runs with those.
                kw.update(
                    heston=sv.heston,
                    K_grid=m.K_grid,
                    T_grid=m.T_grid,
                    leverage_flat=list(sv.leverage_flat),
                )
    try:
        result = qm.simulate_model_paths(req.model, spot, req.rate, dividend, **kw)
    except (RuntimeError, ValueError) as exc:
        raise HTTPException(status_code=422, detail=str(exc)) from exc
    return SimulationPathsResponse(
        model=SimulationModel(req.model),
        time_grid=result["time_grid"],
        paths=result["paths"],
    )


@router.post("/calibrate", response_model=SimulationCalibrateResponse)
def calibrate(req: SimulationCalibrateRequest) -> SimulationCalibrateResponse:
    """Black-Scholes and SABR: fetch the ticker's chain (DB-first, live
    yfinance fallback -- kept on purpose for these two, see CLAUDE.md), run
    the existing SVI vol-surface pipeline, pick the calibrated slice closest
    to the requested maturity, and either read its ATM vol off
    (Black-Scholes) or fit SABR to it (see
    vol_surface.sabr_quotes_from_svi_slice). Local vol, Heston and SLV are
    calibrated on the stored chain only (_calibrate_surface_model)."""
    if req.model in (
        SimulationModel.local_vol,
        SimulationModel.heston,
        SimulationModel.slv,
    ):
        return _calibrate_surface_model(req)
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
    cal = qm.calibrate_sabr_slice(
        quotes=quotes, forward=forward, ttm=slc["ttm"], beta=req.beta
    )

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
