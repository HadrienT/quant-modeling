import math
from typing import Dict, List, Optional

from fastapi import APIRouter, Depends, HTTPException, Query
import requests

from ..cache import TTLCache
from ..dependencies import fred_api_key, require_api_key
from ..logging_utils import get_logger
from ..request_context import set_cache_hit
from ..schemas import RatesCurveResponse

router = APIRouter()
logger = get_logger()

_RATES_CURVE_CACHE = TTLCache[str, RatesCurveResponse](max_size=8, ttl_seconds=60 * 60)
_RATES_ZERO_POINTS_CACHE = TTLCache[str, List[dict]](max_size=4, ttl_seconds=60 * 60)

_FRED_OBS_URL = "https://api.stlouisfed.org/fred/series/observations"
_CURVE_SERIES: Dict[str, List[tuple[float, str]]] = {
    "SOFR": [
        (1.0 / 360.0, "SOFR"),
        (1.0 / 12.0, "SOFR30DAYAVG"),
        (0.25, "SOFR90DAYAVG"),
        (0.5, "SOFR180DAYAVG"),
        (1.0, "DGS1"),
        (2.0, "DGS2"),
        (3.0, "DGS3"),
        (5.0, "DGS5"),
        (7.0, "DGS7"),
        (10.0, "DGS10"),
        (20.0, "DGS20"),
        (30.0, "DGS30"),
    ],
    "OIS": [
        (1.0 / 12.0, "DGS1MO"),
        (0.25, "DGS3MO"),
        (0.5, "DGS6MO"),
        (1.0, "DGS1"),
        (2.0, "DGS2"),
        (3.0, "DGS3"),
        (5.0, "DGS5"),
        (7.0, "DGS7"),
        (10.0, "DGS10"),
        (20.0, "DGS20"),
        (30.0, "DGS30"),
    ],
}


def _normalize_fixed_period_years(value: float) -> float:
    return round(value, 6)


def _fetch_zero_points(curve: str, api_key: str) -> tuple[List[dict], List[str]]:
    series = _CURVE_SERIES[curve]
    zero_points: List[dict] = []
    missing: List[str] = []

    for tenor, series_id in series:
        try:
            rate_value = _latest_fred_observation(series_id, api_key)
        except requests.RequestException as exc:
            logger.warning(
                "rates_curve series_fetch_failed",
                extra={"curve": curve, "series": series_id, "error": str(exc)[:120]},
            )
            rate_value = None
        if rate_value is None:
            missing.append(series_id)
            continue
        zero_points.append({"x": tenor, "y": rate_value})

    return sorted(zero_points, key=lambda p: p["x"]), missing


def _interpolate_linear(points: List[dict], x: float) -> float:
    if not points:
        raise ValueError("No points to interpolate")
    sorted_points = sorted(points, key=lambda p: p["x"])
    if x <= sorted_points[0]["x"]:
        return float(sorted_points[0]["y"])
    if x >= sorted_points[-1]["x"]:
        return float(sorted_points[-1]["y"])

    for idx in range(1, len(sorted_points)):
        left = sorted_points[idx - 1]
        right = sorted_points[idx]
        if left["x"] <= x <= right["x"]:
            if right["x"] == left["x"]:
                return float(left["y"])
            w = (x - left["x"]) / (right["x"] - left["x"])
            return float(left["y"] + w * (right["y"] - left["y"]))

    return float(sorted_points[-1]["y"])


def _compute_forward_curve(zero_points: List[dict], fixed_period_years: float) -> List[dict]:
    if len(zero_points) < 2 or fixed_period_years <= 0:
        return []

    sorted_zero = sorted(zero_points, key=lambda p: p["x"])
    max_tenor = float(sorted_zero[-1]["x"])
    cutoff = max_tenor - fixed_period_years
    if cutoff <= float(sorted_zero[0]["x"]):
        return []

    start_tenors = [float(point["x"]) for point in sorted_zero if float(point["x"]) <= cutoff]
    if not any(abs(t - cutoff) < 1e-10 for t in start_tenors):
        start_tenors.append(cutoff)
    start_tenors = sorted(set(start_tenors))

    forwards: List[dict] = []
    for t_start in start_tenors:
        t_end = t_start + fixed_period_years
        z_start = _interpolate_linear(sorted_zero, t_start)
        z_end = _interpolate_linear(sorted_zero, t_end)
        fwd = (z_end * t_end - z_start * t_start) / fixed_period_years
        if math.isfinite(fwd):
            forwards.append({"x": t_start, "y": fwd})
    return forwards


def _latest_fred_observation(series_id: str, api_key: str) -> Optional[float]:
    response = requests.get(
        _FRED_OBS_URL,
        params={
            "series_id": series_id,
            "api_key": api_key,
            "file_type": "json",
            "sort_order": "desc",
            "limit": 200,
        },
        timeout=8,
    )
    response.raise_for_status()
    observations = response.json().get("observations", [])
    for item in observations:
        raw_value = item.get("value")
        if raw_value in (None, "."):
            continue
        try:
            value = float(raw_value)
        except (TypeError, ValueError):
            continue
        if not math.isnan(value):
            return value
    return None


@router.get("/market/rates/curve", response_model=RatesCurveResponse, dependencies=[Depends(require_api_key)])
def rates_curve(
    curve: str = Query("SOFR", pattern="^(SOFR|OIS)$"),
    curve_type: str = Query("zero", pattern="^(zero|forward)$"),
    fixed_period_years: float = Query(0.5, gt=0.0, le=10.0),
) -> RatesCurveResponse:
    fixed_period_years = _normalize_fixed_period_years(fixed_period_years)
    cache_key = f"{curve}:{curve_type}:{fixed_period_years}"
    cached = _RATES_CURVE_CACHE.get(cache_key)
    if cached:
        set_cache_hit()
        logger.info("rates_curve cache_hit", extra={"curve": curve, "curve_type": curve_type, "points": len(cached.zero)})
        return cached

    api_key = fred_api_key()
    base_cached = _RATES_ZERO_POINTS_CACHE.get(curve)
    if base_cached is not None:
        zero_points = [dict(point) for point in base_cached]
        missing: List[str] = []
        set_cache_hit()
        logger.info("rates_curve zero_points_cache_hit", extra={"curve": curve, "points": len(zero_points)})
    else:
        zero_points, missing = _fetch_zero_points(curve, api_key)
        _RATES_ZERO_POINTS_CACHE.set(curve, [dict(point) for point in zero_points])

    if len(zero_points) < 2:
        raise HTTPException(status_code=503, detail=f"Insufficient FRED data for {curve} curve")

    output_points = zero_points
    if curve_type == "forward":
        output_points = _compute_forward_curve(zero_points, fixed_period_years)
        if len(output_points) < 2:
            raise HTTPException(
                status_code=503,
                detail="Insufficient data to compute forward curve with the selected fixed period",
            )

    response = RatesCurveResponse(curve=curve, zero=output_points)
    _RATES_CURVE_CACHE.set(cache_key, response)

    logger.info(
        "rates_curve cache_miss",
        extra={
            "curve": curve,
            "curve_type": curve_type,
            "fixed_period_years": fixed_period_years,
            "zero_points": len(zero_points),
            "output_points": len(output_points),
            "missing_series": missing,
        },
    )
    return response
