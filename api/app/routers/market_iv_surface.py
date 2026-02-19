from datetime import date, datetime
from typing import List, Optional

from fastapi import APIRouter, Depends, HTTPException, Query
import numpy as np
import yfinance as yf
from scipy.interpolate import griddata

from ..cache import TTLCache
from ..dependencies import require_api_key
from ..logging_utils import get_logger
from ..request_context import set_cache_hit
from ..schemas import IVSurfaceResponse

router = APIRouter()
logger = get_logger()

_NO_OPTIONS_DETAIL = "No options chain available"

_IV_SURFACE_CACHE = TTLCache[str, IVSurfaceResponse](max_size=128, ttl_seconds=60 * 10)


def _extract_iv_point(call, ttm: float, min_open_interest: int) -> Optional[tuple[float, float, float]]:
    try:
        strike = float(call.get("strike", 0))
        if strike <= 0:
            return None

        open_interest = call.get("openInterest", 0)
        if open_interest is None or open_interest < min_open_interest:
            return None

        iv = call.get("impliedVolatility")
        if iv is None or (isinstance(iv, float) and (iv <= 0 or iv != iv)):
            return None

        iv_value = float(iv)
        if iv_value <= 0:
            return None
        return (strike, ttm, iv_value)
    except (ValueError, TypeError):
        return None


def _fetch_all_expirations(ticker: str) -> List[str]:
    try:
        tick = yf.Ticker(ticker)
        expirations = tick.options
        if not expirations:
            raise HTTPException(status_code=404, detail=_NO_OPTIONS_DETAIL)
        return expirations
    except Exception as e:
        logger.error(
            "iv_surface fetch_expirations_failed",
            extra={"ticker": ticker, "error": str(e)[:100]},
        )
        raise HTTPException(status_code=503, detail="Yahoo Finance temporarily unavailable")


def _collect_raw_iv_points(ticker: str, expirations: List[str]) -> List[tuple[float, float, float]]:
    raw_points: List[tuple[float, float, float]] = []
    today = date.today()
    tick = yf.Ticker(ticker)
    min_open_interest = 50

    for exp_str in expirations:
        try:
            exp_date = datetime.strptime(exp_str, "%Y-%m-%d").date()
            ttm = max((exp_date - today).days, 0) / 365.0
            if ttm <= 0:
                continue

            opt_chain = tick.option_chain(exp_str)
            if opt_chain.calls is None or opt_chain.calls.empty:
                continue

            for _, call in opt_chain.calls.iterrows():
                point = _extract_iv_point(call, ttm, min_open_interest)
                if point is not None:
                    raw_points.append(point)
        except Exception as e:
            logger.warning(
                "iv_surface expiration_failed",
                extra={"ticker": ticker, "expiration": exp_str, "error": str(e)[:100]},
            )
            continue

    return raw_points


def _interpolate_iv_surface(
    raw_points: List[tuple[float, float, float]],
    num_strikes: int = 20,
    num_maturities: int = 20,
) -> tuple[List[float], List[float], List[List[Optional[float]]]]:
    points_array = np.array(raw_points)
    strikes_raw = points_array[:, 0]
    maturities_raw = points_array[:, 1]
    ivs_raw = points_array[:, 2]

    min_strike = np.min(strikes_raw)
    max_strike = np.max(strikes_raw)
    min_maturity = np.min(maturities_raw)
    max_maturity = np.max(maturities_raw)

    strike_grid = np.linspace(min_strike, max_strike, num_strikes)
    maturity_grid = np.linspace(min_maturity, max_maturity, num_maturities)
    strike_mesh, maturity_mesh = np.meshgrid(strike_grid, maturity_grid)

    points_to_interp = np.column_stack([strike_mesh.ravel(), maturity_mesh.ravel()])
    iv_grid = griddata(
        (strikes_raw, maturities_raw),
        ivs_raw,
        points_to_interp,
        method="linear",
        fill_value=np.nan,
    )

    iv_grid = iv_grid.reshape(strike_mesh.shape)
    strikes_sorted = list(strike_grid)
    maturities_sorted = list(maturity_grid)

    values: List[List[Optional[float]]] = []
    for i in range(num_maturities):
        row = []
        for j in range(num_strikes):
            iv_val = float(iv_grid[i, j]) if not np.isnan(iv_grid[i, j]) else None
            row.append(iv_val)
        values.append(row)

    return strikes_sorted, maturities_sorted, values


@router.get("/market/iv/surface", response_model=IVSurfaceResponse, dependencies=[Depends(require_api_key)])
def iv_surface(
    ticker: str = Query(..., min_length=1),
    surface: str = Query("mid", pattern="^(mid|bid|ask)$"),
) -> IVSurfaceResponse:
    cache_key = f"{ticker}:{surface}"
    cached = _IV_SURFACE_CACHE.get(cache_key)
    if cached:
        set_cache_hit()
        logger.info("iv_surface cache_hit", extra={"ticker": ticker, "surface": surface})
        return cached

    try:
        expirations = _fetch_all_expirations(ticker)
        raw_points = _collect_raw_iv_points(ticker, expirations)

        if not raw_points:
            raise HTTPException(status_code=404, detail=_NO_OPTIONS_DETAIL)

        logger.info(
            "iv_surface interpolating",
            extra={"ticker": ticker, "raw_points": len(raw_points), "grid_size": "20x20"},
        )
        strikes_sorted, maturities_sorted, values = _interpolate_iv_surface(raw_points)

        response = IVSurfaceResponse(
            ticker=ticker,
            surface=surface,
            strikes=strikes_sorted,
            maturities=maturities_sorted,
            values=values,
        )
        _IV_SURFACE_CACHE.set(cache_key, response)
        logger.info(
            "iv_surface cache_miss",
            extra={
                "ticker": ticker,
                "surface": surface,
                "raw_points": len(raw_points),
                "grid_size": "20x20",
                "note": "griddata linear interpolation (not arbitrage-free)",
            },
        )
        return response
    except HTTPException:
        raise
    except Exception as exc:
        logger.error("iv_surface failed", extra={"ticker": ticker, "surface": surface, "error": str(exc)[:200]})
        raise HTTPException(status_code=500, detail="Failed to fetch IV surface")
