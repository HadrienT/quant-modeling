"""The hedging tab (blueprint/wp/24-backtest.md, lot B8): an option delta-hedged
in a simulated world, run as a task with progress and cancellation, recorded
and replayed. The real `quantbacktest` wheel; Black-Scholes worlds, which run
in a fraction of a second."""

from __future__ import annotations

import os
import time

import pytest
from fastapi.testclient import TestClient
from pydantic import ValidationError

os.environ.setdefault("JWT_SECRET", "test-only-secret-not-for-deployment")

from api.app.audit.sinks import (
    InMemorySink,
    reset_sink_for_testing,
    set_sink_for_testing,
)
from api.app import tasks
from api.app.backtest import hedging, record
from api.app.main import app
from api.app.replay import ReplayOutcome

pytest.importorskip("quantbacktest", reason="run scripts/install_quantbacktest.sh")

BS_WORLD = dict(model="black_scholes", spot=100.0, rate=0.03, volatility=0.2)
REQUEST = dict(
    world=BS_WORLD,
    hedges=["black_scholes_fixed", "true_model", "minimum_variance"],
    paths=4000,
    rebalances=26,
    seed=7,
)


@pytest.fixture
def sink():
    memory = InMemorySink()
    set_sink_for_testing(memory)
    yield memory
    reset_sink_for_testing()


@pytest.fixture
def client():
    return TestClient(app)


def _wait(client, task_id: str, timeout: float = 60.0) -> dict:
    deadline = time.time() + timeout
    while time.time() < deadline:
        body = client.get(f"/api/backtest/tasks/{task_id}").json()
        if body["status"] not in ("queued", "running"):
            return body
        time.sleep(0.05)
    raise AssertionError("the task did not finish")


def test_every_hedge_runs_on_the_same_paths_with_its_error():
    r = hedging.run_hedging(hedging.HedgingRequest(**REQUEST))
    assert [h.hedge for h in r.hedges] == REQUEST["hedges"]
    assert r.strike == pytest.approx(100.0) and r.premium > 0
    assert r.coefficients is not None and set(r.coefficients) == {"a", "b", "c"}
    for h in r.hedges:
        for metric in (h.mean, h.standard_deviation, *h.explanation.values()):
            assert (
                metric.standard_error is not None
                and metric.method
                and metric.convention
            )
        # the common bins hold all but the far tails of every hedge
        assert 0.98 * REQUEST["paths"] <= sum(h.histogram) <= REQUEST["paths"]
    assert len(r.bins) == hedging.HISTOGRAM_BINS + 1
    # In a Black-Scholes world at the world's own volatility, the fixed delta
    # is the true model's: the same paths give the same P&L.
    fixed, true = r.hedges[0], r.hedges[1]
    assert fixed.standard_deviation.value == pytest.approx(
        true.standard_deviation.value, rel=1e-9
    )
    assert fixed.gamma_integral is not None and true.gamma_integral is None


def test_a_task_reports_progress_then_its_result_and_is_recorded(client, sink):
    response = client.post("/api/backtest/hedging", json=REQUEST)
    assert response.status_code == 202, response.text
    body = _wait(client, response.json()["id"])
    assert body["status"] == "done" and body["progress"] == 1.0
    assert [h["hedge"] for h in body["hedging"]["hedges"]] == REQUEST["hedges"]
    (event,) = [e for e in sink.events if e.type == "backtest.run"]
    payload = event.model_dump(mode="json")["payload"]
    assert payload["kind"] == "hedging" and list(payload["fingerprints"]) == ["world"]
    assert payload["headline"]["true_model.standard_deviation"] > 0


def test_a_recorded_hedging_run_replays_to_the_bit(client, sink):
    response = client.post("/api/backtest/hedging", json=REQUEST)
    _wait(client, response.json()["id"])
    (event,) = [e for e in sink.events if e.type == "backtest.run"]
    result = record.replay(event.model_dump(mode="json"))
    assert result.outcome == ReplayOutcome.REPRODUCED
    assert result.differences == []


def test_a_task_can_be_cancelled(client):
    slow = dict(
        world=dict(model="heston"), hedges=["true_model"], paths=10_000, rebalances=252
    )
    task_id = client.post("/api/backtest/hedging", json=slow).json()["id"]
    assert client.delete(f"/api/backtest/tasks/{task_id}").status_code == 200
    assert _wait(client, task_id)["status"] == "cancelled"


def test_the_queue_is_bounded(monkeypatch):
    monkeypatch.setattr(tasks, "MAX_PENDING", 0)
    with pytest.raises(tasks.QueueFull):
        tasks.submit("hedging", lambda t: None)


def test_an_unknown_task_is_a_404(client):
    assert client.get("/api/backtest/tasks/nope").status_code == 404


def test_requests_are_checked():
    with pytest.raises(ValidationError, match="at most 10,000 paths"):
        hedging.HedgingRequest(world=dict(model="heston"), paths=20_000)
    with pytest.raises(ValidationError, match="each hedge at most once"):
        hedging.HedgingRequest(hedges=["true_model", "true_model"])
    with pytest.raises(ValidationError, match="Heston world"):
        hedging.HedgingRequest(world=dict(model="black_scholes", ticker="SPY"))
