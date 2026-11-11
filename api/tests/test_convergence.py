"""Monte-Carlo convergence study (convergence.py, POST <route>/convergence).

Properties: the study prices N/32 ... N/2 and not N, which is the route's
own pricing, so it costs less than one pricing more; with that pricing as
its last point, plain Monte Carlo measures a slope of -1/2 within its
regression error; every Monte-Carlo product has the route; a
non-Monte-Carlo engine is refused rather than given a made-up curve.
"""

from __future__ import annotations

import os

os.environ.setdefault("JWT_SECRET", "test-only-secret-not-for-deployment")

import math

from fastapi.routing import APIRoute
from fastapi.testclient import TestClient

from api.app import convergence
from api.app.main import app
from api.app.routers.pricing import MC_ROUTES
from api.app.schemas import DatedAsianRequest

client = TestClient(app)
BS = dict(
    spot=100.0, strike=100.0, maturity=1.0, rate=0.03, dividend=0.01, vol=0.2,
    is_call=True,
)  # fmt: skip


def post(path: str, body: dict):
    return client.post(path, json=body)


def fit(points: list) -> tuple:
    """Least-squares slope of ln(std error) on ln(paths) and its standard
    error: the fit the page makes (convergenceSlope.ts)."""
    xy = [(math.log(n), math.log(e)) for n, e in points]
    m = len(xy)
    mx, my = sum(x for x, _ in xy) / m, sum(y for _, y in xy) / m
    sxx = sum((x - mx) ** 2 for x, _ in xy)
    slope = sum((x - mx) * (y - my) for x, y in xy) / sxx
    rss = sum((y - my - slope * (x - mx)) ** 2 for x, y in xy)
    return slope, math.sqrt(rss / (m - 2) / sxx)


def test_the_study_prices_the_halvings_not_the_pricing_itself():
    body = {**BS, "engine": "mc", "n_paths": 64_000, "seed": 7}
    r = post("/price/option/vanilla/convergence", body)
    assert r.status_code == 200, r.text
    assert r.json()["n_paths"] == 64_000
    points = r.json()["points"]
    assert [p["paths"] for p in points] == [2_000, 4_000, 8_000, 16_000, 32_000]
    assert [p["seed"] for p in points] == [12, 11, 10, 9, 8]
    # Under one pricing more: N/2 + ... + N/32 < N.
    assert sum(p["paths"] for p in points) < 64_000


def test_with_the_pricing_as_last_point_the_sobol_default_beats_one_half():
    # Sobol RQMC is the default sampler (WP 26 P1c): the ladder's error falls
    # faster than plain Monte-Carlo's 1/sqrt(N) (measured: slope -0.86 +- 0.07
    # on 2026-11-11); the page says "faster than 1/√N".
    body = {**BS, "engine": "mc", "n_paths": 320_000, "seed": 1}
    priced = post("/price/option/vanilla", body).json()
    points = post("/price/option/vanilla/convergence", body).json()["points"]
    ladder = [(p["paths"], p["std_error"]) for p in points]
    ladder.append((320_000, priced["mc_std_error"]))
    assert [n for n, _ in ladder] == [10_000, 20_000, 40_000, 80_000, 160_000, 320_000]
    slope, se = fit(ladder)
    assert se > 0
    assert slope < -0.6
    # Each estimate is within a few of its own standard errors of the price.
    for p in points:
        assert abs(p["npv"] - priced["npv"]) < 5 * p["std_error"]


def test_the_ladder_stops_at_the_request_models_floor():
    req = DatedAsianRequest(
        spot=100.0, rate=0.03, vol=0.2, strike=100.0,
        fixing_dates=["2030-01-01"], n_paths=3_000,
    )  # fmt: skip
    assert [r.n_paths for r in convergence.ladder(req)] == [1_500]


def test_every_monte_carlo_product_has_its_convergence_route():
    assert sorted(MC_ROUTES.values()) == sorted(convergence.MC_PRODUCTS)
    paths = {r.path for r in app.routes if isinstance(r, APIRoute)}
    for path in MC_ROUTES:
        assert path in paths, path
        assert f"{path}/convergence" in paths, path


def test_an_engine_that_is_not_monte_carlo_is_refused():
    r = post("/price/option/vanilla/convergence", {**BS, "engine": "analytic"})
    assert r.status_code == 422
    assert "mc" in r.json()["message"]


def test_the_study_says_the_path_count_the_defaults_gave():
    study = post("/price/option/vanilla/convergence", {**BS, "engine": "mc"}).json()
    assert study["n_paths"] == 200_000
    assert study["points"][-1]["paths"] == 100_000


def test_a_payoff_with_no_variance_has_zero_errors():
    # Deep out of the money: every path pays zero, the error is zero (the
    # page then fits no slope).
    body = {**BS, "strike": 1e6, "engine": "mc", "n_paths": 32_000}
    points = post("/price/option/vanilla/convergence", body).json()["points"]
    assert len(points) == 5
    assert all(math.isclose(p["std_error"], 0.0) for p in points)
