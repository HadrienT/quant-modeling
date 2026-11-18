"""The American option by Longstaff-Schwartz (#313) on the pricing route:
the engine the site offers, the binomial tree its oracle.

Properties: a lower bound for the holder, between the European and the tree,
close to the tree; its five greeks come from the engine, none bumped; a call
without dividend is the European (Merton 1973).
"""

from __future__ import annotations

import os

os.environ.setdefault("JWT_SECRET", "test-only-secret-not-for-deployment")

import pytest
from fastapi.testclient import TestClient

from api.app import valuation
from api.app.main import app
from api.app.schemas import AmericanVanillaRequest

client = TestClient(app)
PUT = dict(
    spot=100.0, strike=100.0, maturity=1.0, rate=0.05, dividend=0.0, vol=0.2,
    is_call=False,
)  # fmt: skip


def price(**body) -> dict:
    r = client.post("/price/option/american-vanilla", json=body)
    assert r.status_code == 200, r.text
    return r.json()


def test_the_put_lies_between_the_european_and_the_tree():
    mc = price(**PUT, engine="mc", n_paths=100_000)
    tree = price(**PUT, engine="binomial", tree_steps=2000)
    european = client.post(
        "/price/option/vanilla", json={**PUT, "engine": "analytic"}
    ).json()
    se = mc["mc_std_error"]
    assert european["npv"] + 4 * se < mc["npv"] < tree["npv"] + 3 * se
    assert mc["npv"] == pytest.approx(tree["npv"], rel=0.01)
    assert "Longstaff-Schwartz" in mc["diagnostics"]
    assert mc["bumped_greeks"] == []
    g = mc["greeks"]
    assert g["delta"] == pytest.approx(tree["greeks"]["delta"], rel=0.02)
    assert g["delta"] < 0 and g["vega"] > 0 and g["rho"] < 0


def test_a_call_without_dividend_is_the_european():
    call = {**PUT, "is_call": True}
    mc = price(**call, engine="mc")
    european = client.post(
        "/price/option/vanilla", json={**call, "engine": "analytic"}
    ).json()
    assert mc["npv"] == pytest.approx(european["npv"], abs=1e-3)
    assert "Merton" in mc["diagnostics"]


def test_the_valuation_record_names_the_engine():
    product = valuation.PRODUCTS["american_vanilla"]
    assert product.engine(AmericanVanillaRequest(**PUT, engine="mc")) == "mc"
    assert product.engine(AmericanVanillaRequest(**PUT)) == "binomial"
