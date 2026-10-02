"""Adjoint greeks of a scripted payoff, as the scripting page asks for them:
they come back with their standard errors, and a payoff that jumps at a level
of the spot is flagged when nothing smooths the jump — its path-by-path
derivative is zero, which is not its delta."""

from __future__ import annotations

import os

os.environ.setdefault("JWT_SECRET", "test-only-secret-not-for-deployment")

import math

from fastapi.testclient import TestClient

from api.app.main import app

client = TestClient(app)

CALL = "2027-10-02\n    pays max(spot() - 100, 0)"
DIGITAL = "2027-10-02\n    if spot() > 100 then pays 10 endif"


def _price(script: str, **extra) -> dict:
    body = {
        **dict(
            script=script,
            model="black_scholes",
            spot=100.0,
            vol=0.2,
            dividend=0.0,
            rate=0.03,
            valuation_date="2026-10-02",
            n_paths=50_000,
            seed=1,
            greeks_method="aad",
        ),
        **extra,
    }
    r = client.post("/price/scripted", json=body)
    assert r.status_code == 200, r.text
    return r.json()


def _codes(priced: dict) -> list[str]:
    return [w["code"] for w in priced["warnings"]]


def test_a_call_has_its_black_scholes_delta_within_its_error():
    priced = _price(CALL)
    risks = {r["label"]: r for r in priced["risks"]}
    assert set(risks) == {"spot", "rate", "div", "vol"}
    d1 = (0.03 + 0.5 * 0.2**2) / 0.2
    delta = 0.5 * (1.0 + math.erf(d1 / math.sqrt(2.0)))
    assert abs(risks["spot"]["value"] - delta) < 4 * risks["spot"]["std_error"] + 1e-3
    assert risks["vol"]["value"] > 0 and risks["div"]["value"] < 0
    assert "greeks_hard_threshold" not in _codes(priced)


def test_no_greeks_unless_asked():
    priced = _price(CALL, greeks_method="none")
    assert not priced["risks"]
    assert all(v is None for v in priced["greeks"].values())


def test_an_unsmoothed_digital_is_flagged_and_its_delta_reads_zero():
    priced = _price(DIGITAL)
    risks = {r["label"]: r for r in priced["risks"]}
    # What the warning is about: a zero that is not the digital's delta.
    assert risks["spot"]["value"] == 0.0 and risks["spot"]["std_error"] == 0.0
    assert _codes(priced)[0] == "greeks_hard_threshold"
    assert "Fuzzy" in priced["warnings"][0]["message"]


def test_smoothing_gives_the_digital_a_delta_and_lifts_the_flag():
    priced = _price(DIGITAL, fuzzy=True)
    spot = next(r for r in priced["risks"] if r["label"] == "spot")
    assert spot["value"] > 2 * spot["std_error"] > 0
    assert "greeks_hard_threshold" not in _codes(priced)


def test_the_flag_is_about_greeks_only():
    assert "greeks_hard_threshold" not in _codes(_price(DIGITAL, greeks_method="none"))
