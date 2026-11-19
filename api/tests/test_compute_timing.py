"""Where a pricing's server time goes (compute_timing.py, PricingResponse.timings).

Properties: phases are exclusive (a nested phase is not counted twice), so
market + calibration + engine + other is the compute time; a pricing on
typed inputs reads no market and spends nothing calibrating; outside a
collector a phase records nothing.
"""

import os
import time
from dataclasses import replace

os.environ.setdefault("JWT_SECRET", "test-only-secret-not-for-deployment")

import pytest  # noqa: E402
from fastapi.testclient import TestClient  # noqa: E402

# One module tree (api.app) throughout: the router's collector and the
# phases below must be the same context variable.
from api.app import compute_timing as ct  # noqa: E402
from api.app import valuation  # noqa: E402
from api.app.main import app  # noqa: E402
from api.app.schemas import PricingResponse, VanillaRequest  # noqa: E402

client = TestClient(app)


def test_a_nested_phase_counts_once():
    with ct.collect() as phases:
        with ct.phase("market"):
            time.sleep(0.02)
            with ct.phase("calibration"):
                time.sleep(0.03)
    assert phases["calibration"] == pytest.approx(0.03, abs=0.015)
    # the market's own time, its nested calibration taken out
    assert phases["market"] == pytest.approx(0.02, abs=0.015)
    assert phases["engine"] == 0.0


def test_outside_a_collector_a_phase_records_nothing():
    with ct.phase("engine"):
        pass
    with ct.collect() as phases:
        pass
    assert sum(phases.values()) == 0.0


def test_the_phases_add_up_to_the_compute_time(monkeypatch):
    """A product whose pricer reads a market and calibrates: what remains of
    its call is the engine, and the router's `other` closes the sum."""

    def price(_req):
        with ct.phase("market"):
            time.sleep(0.02)
        with ct.phase("calibration"):
            time.sleep(0.01)
        time.sleep(0.015)
        return PricingResponse(npv=1.0, greeks={}, diagnostics="", mc_std_error=0.0)

    product = replace(valuation.PRODUCTS["vanilla"], price=price)
    monkeypatch.setitem(valuation.PRODUCTS, "vanilla", product)
    req = VanillaRequest(
        spot=100.0,
        strike=100.0,
        maturity=1.0,
        rate=0.0,
        dividend=0.0,
        vol=0.2,
        is_call=True,
    )
    priced = valuation.price("vanilla", req)
    assert priced.phases["market"] == pytest.approx(0.02, abs=0.015)
    assert priced.phases["calibration"] == pytest.approx(0.01, abs=0.015)
    assert priced.phases["engine"] == pytest.approx(0.015, abs=0.015)
    assert sum(priced.phases.values()) <= priced.duration_s


def test_a_typed_pricing_spends_its_time_in_the_engine():
    body = client.post(
        "/price/scripted-product",
        json={
            "product": "digital-call",
            "spot": 100.0,
            "vol": 0.2,
            "dividend": 0.0,
            "rate": 0.04,
            "model": "black_scholes",
            "n_paths": 4096,
        },
    ).json()
    t = body["timings"]
    assert t["market_ms"] == 0.0 and t["calibration_ms"] == 0.0
    assert t["engine_ms"] > 0.0
    total = t["market_ms"] + t["calibration_ms"] + t["engine_ms"] + t["other_ms"]
    assert total == pytest.approx(body["compute_ms"], rel=1e-9, abs=1e-9)


def test_bumped_greeks_are_timed_apart():
    body = client.post(
        "/price/option/digital",
        json={
            "spot": 100.0,
            "strike": 100.0,
            "maturity": 1.0,
            "rate": 0.04,
            "dividend": 0.0,
            "vol": 0.2,
            "is_call": True,
        },
    ).json()
    t = body["timings"]
    assert t["greeks_ms"] >= 0.0
    total = t["market_ms"] + t["calibration_ms"] + t["engine_ms"] + t["other_ms"]
    assert total == pytest.approx(body["compute_ms"], rel=1e-9, abs=1e-9)
