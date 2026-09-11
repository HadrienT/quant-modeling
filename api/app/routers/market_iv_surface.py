"""
Router — Raw implied-volatility surface: scatter griddata'd onto a regular
mesh, no fitting, no arbitrage guarantee. This is the "brute" point on
blueprint/wp/08-market-data.md's brute / nettoyée / local-vol progression;
/api/local-vol/iv-surface (SVI-fitted) and /api/local-vol/surface (Dupire)
are the other two, in routers/local_vol_pricing.py.

Used to hit yfinance with its own independent fetch (_fetch_all_expirations /
_collect_raw_iv_points) -- an entirely separate implementation from
api/app/local_vol/fetcher.py, drifted apart from it over time. Both now
share vol_surface.fetch_option_chain: one fetch path (stored data-ingest
snapshot, else live yfinance), not two.
"""

from typing import List, Optional

import numpy as np
from fastapi import APIRouter, HTTPException, Query
from scipy.interpolate import griddata

from .. import vol_surface
from ..cache import TTLCache
from ..logging_utils import get_logger
from ..request_context import set_cache_hit
from ..schemas import IVSurfaceResponse

router = APIRouter()
logger = get_logger()

_NO_OPTIONS_DETAIL = "No options chain available"
_MIN_OPEN_INTEREST = 50

_IV_SURFACE_CACHE = TTLCache[str, IVSurfaceResponse](max_size=128, ttl_seconds=60 * 10)


def _raw_iv_points(ticker: str) -> List[tuple]:
    """(strike, ttm, iv) for every liquid call with a usable implied vol.

    Calls only, matching the old implementation -- put-call parity gives
    the same surface, and RawVolSurface's own butterfly check draws the same
    line for the same reason (market/raw_vol_surface.hpp).
    """
    quotes = vol_surface.fetch_option_chain(ticker)
    return [
        (q.strike, q.ttm, q.implied_vol)
        for q in quotes
        if q.is_call and q.has_iv and q.open_interest >= _MIN_OPEN_INTEREST
    ]


def _interpolate_iv_surface(
    raw_points: List[tuple],
    num_strikes: int = 40,
    num_maturities: int = 30,
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

    # Fill NaN (outside convex hull) with nearest-neighbour so the surface is complete
    nan_mask = np.isnan(iv_grid)
    if nan_mask.any():
        iv_nn = griddata(
            (strikes_raw, maturities_raw),
            ivs_raw,
            points_to_interp,
            method="nearest",
        ).reshape(strike_mesh.shape)
        iv_grid = np.where(nan_mask, iv_nn, iv_grid)

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


@router.get("/market/iv/surface", response_model=IVSurfaceResponse)
def iv_surface(
    ticker: str = Query(..., min_length=1),
    surface: str = Query("mid", pattern="^(mid|bid|ask)$"),
) -> IVSurfaceResponse:
    ticker = ticker.upper().strip()
    cache_key = f"{ticker}:{surface}"
    cached = _IV_SURFACE_CACHE.get(cache_key)
    if cached:
        set_cache_hit()
        logger.info(
            "iv_surface cache_hit", extra={"ticker": ticker, "surface": surface}
        )
        return cached

    try:
        raw_points = _raw_iv_points(ticker)
        if not raw_points:
            raise HTTPException(status_code=404, detail=_NO_OPTIONS_DETAIL)

        logger.info(
            "iv_surface interpolating",
            extra={
                "ticker": ticker,
                "raw_points": len(raw_points),
                "grid_size": "40x30",
            },
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
                "note": "griddata linear interpolation (not arbitrage-free) -- "
                "see /api/local-vol/iv-surface for the SVI-fitted, arbitrage-checked surface",
            },
        )
        return response
    except HTTPException:
        raise
    except RuntimeError as exc:
        logger.error(
            "iv_surface fetch_failed", extra={"ticker": ticker, "error": str(exc)[:200]}
        )
        raise HTTPException(
            status_code=503, detail="Yahoo Finance temporarily unavailable"
        ) from exc
    except Exception as exc:
        logger.error(
            "iv_surface failed",
            extra={"ticker": ticker, "surface": surface, "error": str(exc)[:200]},
        )
        raise HTTPException(status_code=500, detail="Failed to fetch IV surface")
