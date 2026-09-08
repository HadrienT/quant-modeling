import math
from typing import Dict, List, Optional

from fastapi import APIRouter, HTTPException, Query

from .. import db
from ..cache import TTLCache
from ..logging_utils import get_logger
from ..request_context import set_cache_hit
from ..schemas import RatesCurveResponse

router = APIRouter()
logger = get_logger()

_RATES_CURVE_CACHE = TTLCache[str, RatesCurveResponse](max_size=12, ttl_seconds=60 * 60)
_RATES_ZERO_POINTS_CACHE = TTLCache[str, List[dict]](max_size=6, ttl_seconds=60 * 60)

# Curve definitions — what each one represents:
#
# Treasury (CMT): Constant Maturity Treasury par yields from FRED. The true
#   risk-free benchmark for government bond pricing.  Full tenor coverage
#   from 1-month to 30-year.
#
# SOFR: Secured Overnight Financing Rate — the post-LIBOR standard for
#   derivatives discounting.  The short end (overnight to 180-day) uses
#   actual SOFR fixings from FRED.  For tenors >= 1Y, FRED does not publish
#   SOFR swap rates, so we fall back to Treasury CMT yields as a proxy.
#   A production desk would source SOFR swap rates from Bloomberg/Refinitiv.
#
# Fed Funds: Effective Federal Funds Rate (EFFR) — the rate banks charge each
#   other for overnight reserve lending.  Pre-reform, OIS swaps referenced
#   this rate.  Short end uses EFFR; beyond overnight the curve is extended
#   with Treasury CMT proxies (same FRED limitation as SOFR).

_CURVE_SERIES: Dict[str, List[tuple[float, str]]] = {
    "Treasury": [
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
    "SOFR": [
        (1.0 / 360.0, "SOFR"),  # overnight SOFR fixing
        (1.0 / 12.0, "SOFR30DAYAVG"),  # 30-day average SOFR
        (0.25, "SOFR90DAYAVG"),  # 90-day average SOFR
        (0.5, "SOFR180DAYAVG"),  # 180-day average SOFR
        # --- proxy zone: Treasury CMT used for 1Y+ (SOFR swaps not on FRED) ---
        (1.0, "DGS1"),
        (2.0, "DGS2"),
        (3.0, "DGS3"),
        (5.0, "DGS5"),
        (7.0, "DGS7"),
        (10.0, "DGS10"),
        (20.0, "DGS20"),
        (30.0, "DGS30"),
    ],
    "FedFunds": [
        (1.0 / 360.0, "EFFR"),  # Effective Federal Funds Rate
        # --- proxy zone: Treasury CMT used beyond overnight ---
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


def _fetch_zero_points(curve: str) -> tuple[List[dict], List[str]]:
    """Every curve tenor comes from the local macro store (data-ingest's
    fred-macro source). Fully local: no live FRED call."""
    zero_points: List[dict] = []
    missing: List[str] = []
    for tenor, series_id in _CURVE_SERIES[curve]:
        try:
            value = db.fred_latest_value(series_id)
        except db.StoreUnavailable as exc:
            raise HTTPException(
                status_code=503, detail="Rates store unavailable"
            ) from exc
        if value is None:
            missing.append(series_id)
            continue
        zero_points.append({"x": tenor, "y": value})
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


def _compute_forward_curve(
    zero_points: List[dict], fixed_period_years: float
) -> List[dict]:
    if len(zero_points) < 2 or fixed_period_years <= 0:
        return []

    sorted_zero = sorted(zero_points, key=lambda p: p["x"])
    max_tenor = float(sorted_zero[-1]["x"])
    cutoff = max_tenor - fixed_period_years
    if cutoff <= float(sorted_zero[0]["x"]):
        return []

    start_tenors = [
        float(point["x"]) for point in sorted_zero if float(point["x"]) <= cutoff
    ]
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


@router.get("/market/rates/curve", response_model=RatesCurveResponse)
def rates_curve(
    curve: str = Query("Treasury", pattern="^(Treasury|SOFR|FedFunds)$"),
    curve_type: str = Query("zero", pattern="^(zero|forward)$"),
    fixed_period_years: float = Query(0.5, gt=0.0, le=10.0),
) -> RatesCurveResponse:
    fixed_period_years = _normalize_fixed_period_years(fixed_period_years)
    cache_key = f"{curve}:{curve_type}:{fixed_period_years}"
    cached = _RATES_CURVE_CACHE.get(cache_key)
    if cached:
        set_cache_hit()
        logger.info(
            "rates_curve cache_hit",
            extra={
                "curve": curve,
                "curve_type": curve_type,
                "points": len(cached.zero),
            },
        )
        return cached

    base_cached = _RATES_ZERO_POINTS_CACHE.get(curve)
    if base_cached is not None:
        zero_points = [dict(point) for point in base_cached]
        missing: List[str] = []
        set_cache_hit()
        logger.info(
            "rates_curve zero_points_cache_hit",
            extra={"curve": curve, "points": len(zero_points)},
        )
    else:
        zero_points, missing = _fetch_zero_points(curve)
        _RATES_ZERO_POINTS_CACHE.set(curve, [dict(point) for point in zero_points])

    if len(zero_points) < 2:
        raise HTTPException(
            status_code=503,
            detail=(
                f"The {curve} curve series are not in the local store yet. "
                "Run: docker compose run --rm ingest run fred-macro --full"
            ),
        )

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
