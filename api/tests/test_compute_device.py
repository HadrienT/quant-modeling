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


# ── Scripts on the GPU (blueprint WP 19, lot G2) ────────────────────────────

SCRIPT = {
    "script": "2027-01-04\n    x = spot()\n2027-06-04\n    pays max(spot() - x, 0)\n",
    "model": "black_scholes",
    "spot": 100.0,
    "vol": 0.25,
    "rate": 0.03,
    "valuation_date": "2026-06-01",
    "n_paths": 50_000,
    "seed": 3,
}


def price_script(c: TestClient, **overrides):
    return c.post("/price/scripted", json={**SCRIPT, **overrides})


def test_scripts_default_to_the_cpu(client):
    c, sink = client
    resp = price_script(c)
    assert resp.status_code == 200, resp.text
    assert resp.json()["device"] == "cpu"
    event = next(e for e in sink.events if e.type == "pricing.valuation")
    assert event.payload["engine"]["device"] == "cpu"


@pytest.mark.skipif(HAS_GPU, reason="the server has a GPU")
def test_script_on_a_missing_gpu_is_a_clear_422(client):
    c, _ = client
    resp = price_script(c, device="gpu")
    assert resp.status_code == 422
    assert "no usable CUDA device" in resp.json()["message"]


@pytest.mark.skipif(not HAS_GPU, reason="no CUDA device")
def test_script_on_the_gpu_gives_the_cpu_philox_price(client):
    c, _ = client
    gpu = price_script(c, device="gpu").json()
    cpu = price_script(c, device="cpu", rng="philox").json()
    assert gpu["device"] == "gpu" and cpu["device"] == "cpu"
    assert "on GPU" in gpu["diagnostics"]
    assert gpu["npv"] == pytest.approx(cpu["npv"], rel=1e-10)
    assert gpu["mc_std_error"] == pytest.approx(cpu["mc_std_error"], rel=1e-8)


@pytest.mark.skipif(not HAS_GPU, reason="no CUDA device")
def test_script_with_sobol_on_auto_now_takes_the_gpu(client):
    # Sobol stayed on the CPU until the GPU got its points and bridge (#102).
    c, _ = client
    body = price_script(c, device="auto", sampler="sobol").json()
    assert body["device"] == "gpu"
    assert "Sobol RQMC" in body["diagnostics"]


# ── Variance reduction and risks on the GPU (blueprint WP 19, lot G3) ──────


def test_script_variance_reduction_is_unbiased_and_says_so(client):
    c, _ = client
    plain = price_script(c, n_paths=100_000).json()
    for extra, marker in (
        ({"control_variate": True}, "spot control"),
        ({"sampler": "stratified"}, "stratified W(T)"),
        ({"sampler": "stratified", "control_variate": True}, "within-stratum"),
    ):
        body = price_script(c, n_paths=100_000, rng="philox", **extra).json()
        assert marker in body["diagnostics"], body["diagnostics"]
        se = (plain["mc_std_error"] ** 2 + body["mc_std_error"] ** 2) ** 0.5
        assert abs(body["npv"] - plain["npv"]) < 4 * se


@pytest.mark.skipif(not HAS_GPU, reason="no CUDA device")
def test_script_variance_reduction_on_the_gpu_gives_the_cpu_numbers(client):
    c, _ = client
    for extra in ({"control_variate": True}, {"sampler": "stratified"}):
        gpu = price_script(c, device="gpu", **extra).json()
        cpu = price_script(c, device="cpu", rng="philox", **extra).json()
        assert gpu["device"] == "gpu"
        assert gpu["npv"] == pytest.approx(cpu["npv"], rel=1e-10)
        assert gpu["mc_std_error"] == pytest.approx(cpu["mc_std_error"], rel=1e-7)


@pytest.mark.skipif(HAS_GPU, reason="the server has a GPU")
def test_script_risks_on_a_missing_gpu(client):
    c, _ = client
    resp = price_script(c, greeks_method="aad", device="gpu")
    assert resp.status_code == 422
    assert "no usable CUDA device" in resp.json()["message"]
    body = price_script(c, greeks_method="aad", device="auto").json()
    assert body["device"] == "cpu"
    assert "on the CPU: " in body["diagnostics"]


@pytest.mark.skipif(not HAS_GPU, reason="no CUDA device")
def test_script_risks_on_the_gpu_are_the_cpu_tapes(client):
    c, sink = client
    # A multiple of the tape's batches of 64 paths: the two means coincide.
    gpu = price_script(c, greeks_method="aad", device="gpu", n_paths=64_000).json()
    cpu = price_script(
        c, greeks_method="aad", device="cpu", rng="philox", n_paths=64_000
    ).json()
    assert gpu["device"] == "gpu" and cpu["device"] == "cpu"
    assert "duals on GPU" in gpu["diagnostics"]
    assert gpu["npv"] == pytest.approx(cpu["npv"], rel=1e-10)
    by_label = {r["label"]: r["value"] for r in cpu["risks"]}
    for r in gpu["risks"]:
        assert r["value"] == pytest.approx(by_label[r["label"]], rel=1e-8, abs=1e-10)
    devices = [
        e.payload["engine"]["device"]
        for e in sink.events
        if e.type == "pricing.valuation"
    ]
    assert devices == ["gpu", "cpu"]


def test_antithetic_pairs_can_be_turned_off(client):
    c, _ = client
    on = price_script(c, rng="philox").json()
    off = price_script(c, rng="philox", antithetic=False).json()
    assert "antithetic" in on["diagnostics"]
    assert "antithetic" not in off["diagnostics"]
    se = (on["mc_std_error"] ** 2 + off["mc_std_error"] ** 2) ** 0.5
    assert abs(on["npv"] - off["npv"]) < 4 * se


# ── Sobol on the GPU (issue #102) and importance sampling (issue #103) ──────

DEEP_DIGITAL = "2027-06-01\n    if spot() > 180 then pays 100 endIf\n"


def _digital_bs():
    import math

    T, r, q, s = 1.0, 0.03, 0.0, 0.25  # 2026-06-01 -> 2027-06-01, ACT/365F
    d2 = (math.log(100 / 180) + (r - q - 0.5 * s * s) * T) / (s * math.sqrt(T))
    return 100 * math.exp(-r * T) * 0.5 * math.erfc(-d2 / math.sqrt(2))


def test_importance_sampling_prices_a_rare_digital_tightly(client):
    c, _ = client
    plain = price_script(c, script=DEEP_DIGITAL, n_paths=100_000).json()
    is_ = price_script(
        c, script=DEEP_DIGITAL, n_paths=100_000, importance_sampling=True
    ).json()
    assert "+ importance sampling (drift" in is_["diagnostics"], is_["diagnostics"]
    exact = _digital_bs()
    assert abs(is_["npv"] - exact) < 4 * is_["mc_std_error"]
    assert is_["mc_std_error"] < plain["mc_std_error"] / 3


def test_importance_sampling_says_when_it_steps_aside(client):
    c, _ = client
    body = price_script(c, importance_sampling=True).json()
    diag = body["diagnostics"]
    assert ("+ importance sampling (drift" in diag) != (
        "(no importance sampling:" in diag
    )


@pytest.mark.skipif(not HAS_GPU, reason="no CUDA device")
def test_sobol_on_the_gpu_gives_the_cpu_numbers(client):
    c, _ = client
    gpu = price_script(c, device="gpu", sampler="sobol").json()
    cpu = price_script(c, device="cpu", sampler="sobol").json()
    assert gpu["device"] == "gpu" and "Sobol RQMC" in gpu["diagnostics"]
    assert gpu["npv"] == pytest.approx(cpu["npv"], rel=1e-10)
    assert gpu["mc_std_error"] == pytest.approx(cpu["mc_std_error"], rel=1e-6)


@pytest.mark.skipif(not HAS_GPU, reason="no CUDA device")
def test_risks_under_sobol_on_the_gpu_are_the_tapes(client):
    c, _ = client
    kw = dict(greeks_method="aad", sampler="sobol", n_paths=16 * 2048)
    gpu = price_script(c, device="gpu", **kw).json()
    cpu = price_script(c, device="cpu", **kw).json()
    assert gpu["device"] == "gpu" and "Sobol RQMC" in gpu["diagnostics"]
    assert gpu["npv"] == pytest.approx(cpu["npv"], rel=1e-10)
    by_label = {r["label"]: r["value"] for r in cpu["risks"]}
    for r in gpu["risks"]:
        assert r["value"] == pytest.approx(by_label[r["label"]], rel=1e-8, abs=1e-10)


# ── Two GPUs (blueprint WP 19, lot G4) ──────────────────────────────────────


@pytest.mark.skipif(not HAS_GPU, reason="no CUDA device")
def test_the_response_and_the_audit_say_how_many_gpus(client):
    c, sink = client
    # enough paths for every card to join (a card needs 128 logical blocks)
    body = price_script(c, device="gpu", n_paths=2_200_000).json()
    n = len(qm.gpu_devices())
    assert body["gpus"] == n
    if n > 1:
        assert f"{n} x " in body["diagnostics"]
    event = next(e for e in sink.events if e.type == "pricing.valuation")
    assert event.payload["engine"]["gpus"] == n


def test_the_cpu_reports_no_gpu(client):
    c, sink = client
    body = price_script(c).json()
    assert body["gpus"] == 0
    event = next(e for e in sink.events if e.type == "pricing.valuation")
    assert event.payload["engine"]["gpus"] is None
