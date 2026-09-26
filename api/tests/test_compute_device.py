"""CPU or GPU for a Monte-Carlo pricing (blueprint/wp/19-gpu.md §8).

Runs against the real native module: on the CI (no CUDA build, no GPU) the GPU
cases check the refusal; on the server with a CUDA wheel, they price on the
V100 and compare with the CPU.
Run from the repo root:  pytest api/tests/test_compute_device.py
"""

from __future__ import annotations

import os

os.environ.setdefault("JWT_SECRET", "test-only-secret-not-for-deployment")

import pytest
import quantmodeling as qm
from fastapi.testclient import TestClient

from api.app.audit.sinks import (
    InMemorySink,
    reset_sink_for_testing,
    set_sink_for_testing,
)
from api.app.main import app

HAS_GPU = bool(qm.gpu_devices())

VANILLA = {
    "spot": 100.0,
    "strike": 100.0,
    "maturity": 1.0,
    "rate": 0.05,
    "dividend": 0.02,
    "vol": 0.2,
    "is_call": True,
    "engine": "mc",
    "n_paths": 400_000,
    "seed": 7,
}


@pytest.fixture
def client(monkeypatch, tmp_path):
    monkeypatch.setenv("LOG_DIR", str(tmp_path))
    monkeypatch.setenv("QM_AUDIT_SPOOL_DIR", str(tmp_path / "spool"))
    reset_sink_for_testing()
    sink = InMemorySink()
    set_sink_for_testing(sink)
    try:
        yield TestClient(app), sink
    finally:
        reset_sink_for_testing()


def price(c: TestClient, **overrides):
    return c.post("/price/option/vanilla", json={**VANILLA, **overrides})


def test_devices_endpoint_describes_the_server(client):
    c, _ = client
    body = c.get("/price/devices").json()
    assert body["cpu"]
    assert body["gpu_compiled"] == qm.gpu_compiled()
    assert body["gpus"] == list(qm.gpu_devices())


def test_default_stays_on_the_cpu_and_records_it(client):
    c, sink = client
    resp = price(c)
    assert resp.status_code == 200
    assert resp.json()["device"] == "cpu"
    event = next(e for e in sink.events if e.type == "pricing.valuation")
    assert event.payload["engine"]["device"] == "cpu"


def test_non_mc_engines_ignore_the_device(client):
    c, sink = client
    resp = price(c, engine="analytic", device="gpu")
    assert resp.status_code == 200
    assert resp.json()["device"] == "cpu"
    event = next(e for e in sink.events if e.type == "pricing.valuation")
    assert event.payload["engine"]["device"] is None


def test_auto_takes_the_gpu_only_when_there_is_one(client):
    c, _ = client
    resp = price(c, device="auto")
    assert resp.status_code == 200
    assert resp.json()["device"] == ("gpu" if HAS_GPU else "cpu")


@pytest.mark.skipif(HAS_GPU, reason="the server has a GPU")
def test_gpu_without_a_gpu_is_a_clear_422(client):
    c, _ = client
    resp = price(c, device="gpu")
    assert resp.status_code == 422
    assert "no usable CUDA device" in resp.json()["message"]


@pytest.mark.skipif(not HAS_GPU, reason="no CUDA device")
def test_gpu_gives_the_cpu_philox_price(client):
    c, sink = client
    gpu = price(c, device="gpu").json()
    cpu = price(c, device="cpu", rng="philox").json()
    assert gpu["device"] == "gpu" and cpu["device"] == "cpu"
    # Same draws, same reduction tree: the prices differ by a few ulps only.
    assert gpu["npv"] == pytest.approx(cpu["npv"], rel=1e-12)
    assert gpu["mc_std_error"] == pytest.approx(cpu["mc_std_error"], rel=1e-10)
    devices = [
        e.payload["engine"]["device"]
        for e in sink.events
        if e.type == "pricing.valuation"
    ]
    assert devices == ["gpu", "cpu"]


def test_path_count_is_capped(client):
    c, _ = client
    assert price(c, n_paths=100_000_001).status_code == 422
