"""Quanto option through the API: the closed form it must match (Reiner
1992, written here independently), the valuation record and its replay."""

from __future__ import annotations

import math
import os

os.environ.setdefault("JWT_SECRET", "test-only-secret-not-for-deployment")

import pytest
from fastapi.testclient import TestClient

from api.app import replay, valuation
from api.app.audit.envelope import Event
from api.app.audit.sinks import (
    InMemorySink,
    reset_sink_for_testing,
    set_sink_for_testing,
)
from api.app.main import app

BODY = dict(
    spot=8000.0,
    strike=8200.0,
    maturity=1.5,
    rate_domestic=0.045,
    rate_foreign=0.030,
    dividend=0.025,
    vol=0.18,
    fx_vol=0.08,
    correlation=0.30,
    fx_rate=1.10,
    is_call=True,
)


def reiner(b: dict) -> float:
    t = b["maturity"]
    f = b["spot"] * math.exp(
        (b["rate_foreign"] - b["dividend"] - b["correlation"] * b["vol"] * b["fx_vol"])
        * t
    )
    sd = b["vol"] * math.sqrt(t)
    d1 = (math.log(f / b["strike"]) + 0.5 * sd * sd) / sd
    n = lambda x: 0.5 * math.erfc(-x / math.sqrt(2))  # noqa: E731
    return (
        b["fx_rate"]
        * math.exp(-b["rate_domestic"] * t)
        * (f * n(d1) - b["strike"] * n(d1 - sd))
    )


@pytest.fixture
def sink():
    s = InMemorySink()
    set_sink_for_testing(s)
    yield s
    reset_sink_for_testing()


def test_the_endpoint_prices_the_closed_form_and_records_the_valuation(sink):
    r = TestClient(app).post("/price/option/quanto", json=BODY)
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["npv"] == pytest.approx(reiner(BODY), rel=1e-12)
    assert body["greeks"]["rho"] == pytest.approx(-BODY["maturity"] * body["npv"])
    [event] = [e for e in sink.events if e.type == "pricing.valuation"]
    assert event.payload["product"] == "quanto"
    assert event.payload["model"]["name"] == "quanto_black_scholes"


@pytest.mark.parametrize("engine", ["analytic", "mc"])
def test_a_recorded_quanto_replays_exactly(engine):
    body = {**BODY, "engine": engine, "n_paths": 20_000, "seed": 3}
    req = valuation.PRODUCTS["quanto"].request.model_validate(body)
    priced = valuation.price("quanto", req)
    event = Event.create(
        "pricing.valuation", valuation.payload("quanto", req, priced, ip_hash=None)
    )
    result = replay.replay(event.model_dump(mode="json"))
    assert (
        result.outcome == replay.ReplayOutcome.REPRODUCED
        and result.npv_difference == 0.0
    )


def test_an_impossible_correlation_is_refused():
    r = TestClient(app).post("/price/option/quanto", json={**BODY, "correlation": 1.5})
    assert r.status_code == 422
