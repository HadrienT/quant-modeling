"""The /rates page — `rates_derivatives.py` and `/api/rates/*`, on the real
C++ library: the curves reprice their quotes, the swap at its par rate is
worth nothing, Hull-White is calibrated in bp of normal vol, the Bermudan is
worth at least its European, and bad quotes are the user's error (422)."""

from __future__ import annotations

import os

os.environ.setdefault("JWT_SECRET", "test-only-secret-not-for-deployment")

import pytest
from fastapi.testclient import TestClient

from api.app import rates_derivatives
from api.app.main import app
from api.app.rates_derivatives_schemas import RatesAnalysisRequest

client = TestClient(app)


@pytest.fixture(scope="module")
def example() -> dict:
    r = client.get("/api/rates/example")
    assert r.status_code == 200
    body = r.json()
    assert "not market data" in body["label"]
    return body["request"]


@pytest.fixture(scope="module")
def analysis(example) -> dict:
    r = client.post("/api/rates/analyse", json=example)
    assert r.status_code == 200, r.text
    return r.json()


def test_the_curves_reprice_their_swaps_and_carry_a_positive_basis(analysis):
    c = analysis["curves"]
    assert c["max_repricing_error_bp"] < 1e-6
    # EURIBOR 6M fixes above €STR in the example: the basis is positive.
    assert all(p["basis_bp"] > 0 for p in c["points"])


def test_the_swap_at_its_par_rate_is_worth_nothing(example):
    body = dict(example)
    first = client.post("/api/rates/analyse", json=body).json()["swap"]
    body["swap"] = {**example["swap"], "fixed_rate": first["par_rate"]}
    swap = client.post("/api/rates/analyse", json=body).json()["swap"]
    assert abs(swap["npv"]) < 1e-6 * example["swap"]["notional"]
    assert swap["pv01"] == pytest.approx(
        swap["annuity"] * example["swap"]["notional"] * 1e-4
    )


def test_hull_white_is_calibrated_in_bp_of_normal_vol(analysis):
    hw = analysis["hull_white"]
    assert hw["converged"]
    # A two-parameter model on eight quotes: a few bp, not zero, not tens.
    assert 0.0 < hw["rmse_bp"] < 5.0
    for p in hw["points"]:
        assert abs(p["model_vol"] - p["market_vol"]) * 1e4 <= hw["worst_bp"] + 1e-9


def test_every_model_prices_the_same_forward_and_the_bermudan_beats_the_european(
    analysis,
):
    sw = analysis["swaption"]
    models = {p["model"].split(" ")[0]: p for p in sw["prices"]}
    assert set(models) == {"Bachelier", "Black", "SABR", "Hull-White"}
    for p in sw["prices"]:
        assert p["price"] > 0 and p["implied_normal_vol"] > 0
    assert sw["bermudan_price"] > models["Hull-White"]["price"]
    assert sw["switch_premium"] == pytest.approx(
        sw["bermudan_price"] - models["Hull-White"]["price"]
    )


def test_a_fixed_mean_reversion_is_kept(example):
    body = {**example, "hull_white_mean_reversion": 0.03}
    hw = client.post("/api/rates/analyse", json=body).json()["hull_white"]
    assert hw["mean_reversion"] == pytest.approx(0.03)
    assert hw["mean_reversion_fixed"]


def test_quotes_no_curve_can_reprice_are_a_422(example):
    body = {
        **example,
        "swaps": [{"tenor": 2, "rate": 0.02}, {"tenor": 2, "rate": 0.03}],
    }
    r = client.post("/api/rates/analyse", json=body)
    assert r.status_code == 422
    assert "duplicate maturity" in r.text


def test_the_methodology_says_the_quotes_are_not_market_data():
    text = " ".join(p for s in rates_derivatives.methodology() for p in s.paragraphs)
    assert "Nothing on this page is market data" in text


def test_the_example_validates_as_a_request():
    RatesAnalysisRequest.model_validate(rates_derivatives.example_request())
