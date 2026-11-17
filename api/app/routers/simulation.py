"""
Router — path-simulation endpoints for the simulation page: pick a model
(Black-Scholes, SABR, local vol, Heston, SLV), supply parameters by hand or calibrate
against a real ticker's option chain, see simulated paths.

POST /simulation/paths/black-scholes
POST /simulation/paths/sabr
POST /simulation/paths/model        local vol, Heston, SLV
POST /simulation/calibrate
POST /simulation/distribution  the law of the paths and the prices they imply (WP 29)

Path generation is illustrative, not a pricing engine (see
engines/mc/path_simulation.hpp's doc comments); calibration reuses the
existing DB-first market-data fetch and SVI vol-surface pipeline this
project already has (vol_surface.py, quantmodeling.calibrate_vol_surface).
"""

from __future__ import annotations

import math
from datetime import date
from typing import Any, List, Optional

import quantmodeling as qm
from fastapi import APIRouter, HTTPException

from .. import market_snapshot, simulation_market, stochastic_vol, vol_surface
from ..logging_utils import get_logger
from ..schemas import (
    BarrierOut,
    BSPathRequest,
    DistributionMarket,
    DistributionRequest,
    DistributionResponse,
    HistogramOut,
    MartingaleOut,
    SmileOut,
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
                clamped_mass=sv.leverage_clamped_mass,
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
                    T_grid=list(sv.leverage_T_grid),  # the leverage's own (issue #227)
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


# ── The law of the paths (WP 29) ─────────────────────────────────────────────


def _finite(x: Any) -> Any:
    """NaN and infinities as None, at any depth: JSON has no NaN."""
    if isinstance(x, float):
        return x if math.isfinite(x) else None
    if isinstance(x, list):
        return [_finite(v) for v in x]
    return x


def _grid_index(
    t: float, ttm: float, n: int, lo: int = 1, hi: Optional[int] = None
) -> int:
    return min(max(round(t / ttm * n), lo), n if hi is None else hi)


def _sabr_from_surface(m, rate: float, ttm: float, beta: float) -> dict:
    """SABR fitted to the stored surface's slice nearest ttm, as /calibrate
    does on the chain (but on the database only)."""
    slc = vol_surface.nearest_slice(m.surface_slices(), ttm)
    forward = m.spot * math.exp((rate - m.dividend) * slc["ttm"])
    quotes = vol_surface.sabr_quotes_from_svi_slice(slc, forward)
    if len(quotes) < 3:
        raise HTTPException(
            status_code=422,
            detail=f"Not enough usable points on the stored surface of '{m.ticker}' to fit SABR.",
        )
    cal = qm.calibrate_sabr_slice(
        quotes=quotes, forward=forward, ttm=slc["ttm"], beta=beta
    )
    return {k: cal[k] for k in ("alpha", "beta", "rho", "nu")}


def _distribution_inputs(req: DistributionRequest):
    """The model's native inputs (kwargs of qm.simulate_path_distribution),
    its spot and dividend, and the ticker's stored market if there is one."""
    kw: dict = {}
    m = _market(req.ticker, req.rate) if req.ticker else None
    spot = m.spot if m else req.spot
    dividend = m.dividend if m else req.dividend
    if req.model == SimulationModel.black_scholes:
        vol = req.vol
        if vol is None:  # the stored surface's ATM vol at ttm
            slices = m.surface_slices()
            t = min(max(req.ttm, slices[0]["ttm"]), slices[-1]["ttm"])
            vol = simulation_market.implied_vols(slices, t, [1.0])[0]
            if vol is None:
                raise HTTPException(
                    status_code=422,
                    detail=f"No ATM vol on the stored surface of '{m.ticker}'.",
                )
        kw.update(vol=vol)
    elif req.model == SimulationModel.sabr:
        kw.update(
            sabr=(
                req.sabr.model_dump()
                if req.sabr
                else _sabr_from_surface(m, req.rate, req.ttm, 0.5)
            )
        )
        if m:  # SABR simulates the forward to ttm
            spot = m.spot * math.exp((req.rate - m.dividend) * req.ttm)
    elif req.model == SimulationModel.local_vol:
        kw.update(K_grid=m.K_grid, T_grid=m.T_grid, sigma_loc_flat=m.sigma_loc_flat)
    else:
        if req.heston is not None and (
            m is None or req.model == SimulationModel.heston
        ):
            kw.update(heston=req.heston.model_dump())
        if m is not None and (req.heston is None or req.model == SimulationModel.slv):
            sv = _stochastic(m)
            kw.update(heston=sv.heston)
            if req.model == SimulationModel.slv:
                # The leverage was calibrated with the calibrated Heston
                # parameters: SLV always runs with those.
                kw.update(
                    K_grid=m.K_grid,
                    T_grid=list(sv.leverage_T_grid),
                    leverage_flat=list(sv.leverage_flat),
                )
    return kw, spot, dividend, m


def _smile_out(
    s: dict, start: float, market_iv: Optional[List[Optional[float]]]
) -> SmileOut:
    return SmileOut(
        time=s["ttm"],
        start=start,
        forward=s["forward"],
        moneyness=s["moneyness"],
        strikes=s["strikes"],
        prices=s["prices"],
        price_stderr=s["price_stderr"],
        model_iv=_finite(s["implied_vols"]),
        iv_stderr=_finite(s["iv_stderr"]),
        market_iv=market_iv,
    )


@router.post("/distribution", response_model=DistributionResponse)
def distribution(req: DistributionRequest) -> DistributionResponse:
    """The law of the model's paths and the prices they imply, each with its
    Monte Carlo error (engines/mc/path_distribution.hpp), and, with a ticker,
    the stored SVI surface at the same dates and moneyness
    (simulation_market.py)."""
    kw, spot, dividend, m = _distribution_inputs(req)
    n = req.n_steps
    times = req.smile_times or [req.ttm / 4, req.ttm / 2, req.ttm]
    smile_dates = sorted(
        {_grid_index(t, req.ttm, n) for t in times if 0 < t <= req.ttm}
    )
    if not smile_dates:
        raise HTTPException(status_code=422, detail="smile_times must lie in (0, ttm].")
    forward_start = _grid_index(req.forward_start or req.ttm / 2, req.ttm, n, hi=n - 1)
    try:
        r = qm.simulate_path_distribution(
            req.model.value,
            spot,
            req.rate,
            dividend,
            ttm=req.ttm,
            n_steps=n,
            n_paths=req.n_paths,
            n_display=req.n_display,
            seed=req.seed,
            smile_dates=smile_dates,
            smile_moneyness=req.moneyness,
            forward_start=forward_start,
            barrier=req.barrier,
            barrier_strike=req.barrier_strike,
            **kw,
        )
    except (RuntimeError, ValueError) as exc:
        raise HTTPException(status_code=422, detail=str(exc)) from exc

    tg = r["time_grid"]
    slices = m.surface_slices() if m else []
    # SABR simulates the forward to ttm: only its last date is an option on
    # the spot at that maturity, comparable with the market.
    comparable = (
        (lambda i: i == n) if req.model == SimulationModel.sabr else (lambda i: True)
    )

    smiles = [
        _smile_out(
            s,
            0.0,
            (
                simulation_market.implied_vols(slices, s["ttm"], s["moneyness"])
                if m and comparable(s["date"])
                else None
            ),
        )
        for s in r["smiles"]
    ]
    fs = r["forward_smile"]
    forward_smile = _smile_out(fs, tg[fs["date"]], None) if fs else None

    market_density = None
    arbitrage: List[float] = []
    if m:
        market_density = []
        for i, (lo, hi) in enumerate(zip(r["hist_lo"], r["hist_hi"])):
            t = tg[i]
            if i == 0 or not comparable(i) or hi <= lo:
                market_density.append(None)
                continue
            nb = len(r["hist_counts"][i])
            centres = [lo + (b + 0.5) * (hi - lo) / nb for b in range(nb)]
            forward = m.spot * math.exp((req.rate - m.dividend) * t)
            if not simulation_market.is_density(slices, t, forward, centres):
                arbitrage.append(t)
                market_density.append(None)
                continue
            d = simulation_market.density(slices, t, forward, centres)
            market_density.append(None if all(x is None for x in d) else d)

    barrier = None
    if r["barrier"]:
        b = r["barrier"]
        barrier = BarrierOut(
            barrier=req.barrier * spot,
            strike=req.barrier_strike * spot,
            price=b["price"],
            stderr=b["stderr"],
            vanilla_price=b["vanilla_price"],
            vanilla_stderr=b["vanilla_stderr"],
        )

    return DistributionResponse(
        model=req.model,
        spot=spot,
        rate=req.rate,
        dividend=dividend,
        n_paths=r["n_paths"],
        time_grid=tg,
        paths=r["paths"],
        vols=_finite(r["vols"]) if r["has_vol"] else None,
        quantile_levels=r["quantile_levels"],
        spot_quantiles=r["spot_quantiles"],
        vol_quantiles=_finite(r["vol_quantiles"]) if r["has_vol"] else None,
        martingale=MartingaleOut(
            mean=r["deflated_mean"],
            stderr=r["deflated_stderr"],
            exact=r["deflated_exact"],
        ),
        atm_vol=_finite(r["atm_vol"]),
        atm_vol_stderr=_finite(r["atm_vol_stderr"]),
        histogram=HistogramOut(
            lo=r["hist_lo"],
            hi=r["hist_hi"],
            counts=r["hist_counts"],
            market_density=market_density,
        ),
        smiles=smiles,
        forward_smile=forward_smile,
        barrier=barrier,
        market=(
            DistributionMarket(
                ticker=m.ticker,
                snapshot=m.valuation_date,
                spot=m.spot,
                dividend=m.dividend,
                first_slice=slices[0]["ttm"],
                last_slice=slices[-1]["ttm"],
            )
            if m and slices
            else None
        ),
    )
