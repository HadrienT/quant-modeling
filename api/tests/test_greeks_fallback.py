"""Greeks the engines do not return, completed by bumping
(greeks_fallback.py) on the pricing routes.

Properties: a bumped greek is the engine's own where the engine has one (the
European vanilla, to the size of the bump); what the engine gives is kept and
not named as bumped; a product is given a greek only for an input it has; the
signs are those of the payoff.
"""

from __future__ import annotations

import os

os.environ.setdefault("JWT_SECRET", "test-only-secret-not-for-deployment")

import pytest
from fastapi.testclient import TestClient

from api.app import greeks_fallback, pricing_service
from api.app.main import app
from api.app.schemas import Greeks, VanillaRequest

client = TestClient(app)
BS = dict(
    spot=100.0, strike=100.0, maturity=1.0, rate=0.03, dividend=0.01, vol=0.2,
    is_call=True,
)  # fmt: skip


def price(path: str, **body) -> dict:
    r = client.post(f"/price/{path}", json=body)
    assert r.status_code == 200, r.text
    return r.json()


def test_a_bumped_greek_is_the_engines_own():
    req = VanillaRequest(**BS, engine="analytic")
    own = pricing_service.price_vanilla(req)
    stripped = own.model_copy(update={"greeks": Greeks()})
    completed, names = greeks_fallback.complete(
        req, stripped, lambda r: pricing_service.price_vanilla(r).npv
    )
    assert names == ["delta", "gamma", "vega", "theta", "rho"]
    for name, tolerance in (
        ("delta", 3e-4),
        ("gamma", 1e-3),
        ("vega", 1e-3),
        ("rho", 1e-6),
        ("theta", 2e-3),
    ):
        assert getattr(completed.greeks, name) == pytest.approx(
            getattr(own.greeks, name), rel=tolerance
        ), name


def test_what_the_engine_gives_is_kept_and_only_the_rest_is_bumped():
    analytic = price("option/vanilla", **BS, engine="analytic")
    assert analytic["bumped_greeks"] == []
    # A tree gives four of them: the fifth is a bump, close to the closed
    # form's.
    european = price("option/vanilla", **BS, engine="binomial")
    assert european["bumped_greeks"] == ["rho"]
    assert european["greeks"]["rho"] == pytest.approx(
        analytic["greeks"]["rho"], rel=0.01
    )
    assert european["greeks"]["rho_std_error"] is None
    tree = price("option/american-vanilla", **BS, engine="binomial")
    assert tree["bumped_greeks"] == ["rho"] and tree["greeks"]["rho"] > 0


def test_a_digital_has_its_five_greeks_and_their_signs():
    body = price("option/digital", **BS)
    assert body["bumped_greeks"] == ["delta", "gamma", "vega", "theta", "rho"]
    g = body["greeks"]
    assert g["delta"] > 0  # a call pays above the strike
    # At the money and slightly below the forward, more volatility lowers the
    # chance of ending above the strike (d2 falls).
    assert g["vega"] < 0 and all(g[k] is not None for k in ("gamma", "theta", "rho"))


def test_a_product_has_only_the_greeks_of_its_inputs():
    future = price("future", spot=100, strike=100, maturity=1, rate=0.03, dividend=0.01)
    # A future is linear in the spot and has no volatility.
    assert future["bumped_greeks"] == ["delta", "gamma", "theta", "rho"]
    assert future["greeks"]["vega"] is None
    assert future["greeks"]["delta"] == pytest.approx(0.99, abs=0.02)
    assert future["greeks"]["gamma"] == pytest.approx(0.0, abs=1e-6)
    bond = price("bond/zero-coupon", maturity=5, rate=0.03)
    assert bond["bumped_greeks"] == ["theta", "rho"]
    # Its rho is minus duration times price.
    assert bond["greeks"]["rho"] == pytest.approx(-5 * bond["npv"], rel=1e-3)
    assert bond["greeks"]["theta"] == pytest.approx(0.03 * bond["npv"], rel=1e-2)


def test_a_monte_carlo_product_is_bumped_on_the_same_draws():
    # A worst-of call on performances S_T / S_0, struck at 1.
    body = dict(
        spots=[100, 95], vols=[0.2, 0.25], strike=1.0, maturity=1, rate=0.03,
        is_call=True, n_paths=20000, seed=3,
    )  # fmt: skip
    first = price("option/rainbow", **body)
    assert first["npv"] > 0
    assert first["bumped_greeks"] == ["delta", "gamma", "vega", "theta", "rho"]
    # A performance does not move with the level of the spots; an option
    # gains with the volatility and with the rate its forward grows at.
    assert first["greeks"]["delta"] == pytest.approx(0.0, abs=1e-9)
    assert first["greeks"]["vega"] > 0 and first["greeks"]["rho"] > 0
    # The same seed: the same greeks, to the bit.
    assert price("option/rainbow", **body)["greeks"] == first["greeks"]


def test_rates_and_scripts_keep_their_own_sensitivities():
    from api.app.routers.pricing import _OWN_GREEKS

    assert {"interest_rate_swap", "swaption", "script"} <= _OWN_GREEKS
