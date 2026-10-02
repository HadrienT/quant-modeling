"""Rates derivatives — `rates_derivatives.py`, `/api/rates/*` and the two
pricing endpoints, on the real C++ library: the curves reprice their quotes,
the swap at its par rate is worth nothing, Hull-White is calibrated in bp of
normal vol, the Bermudan is worth at least its European, the model is chosen
and announced, each valuation is recorded and replays, and bad quotes are the
user's error (422)."""

from __future__ import annotations

import os

os.environ.setdefault("JWT_SECRET", "test-only-secret-not-for-deployment")

import pytest
from fastapi.testclient import TestClient

from api.app import rates_derivatives, replay, valuation
from api.app.audit.envelope import Event
from api.app.main import app
from api.app.rates_derivatives_schemas import RatesQuoteSetResponse

client = TestClient(app)


@pytest.fixture(scope="module")
def quotes() -> dict:
    r = client.get("/api/rates/quotes/eur-illustrative")
    assert r.status_code == 200
    body = r.json()
    assert body["source"] == "manual" and body["trades"] is None
    assert "not market data" in body["label"]
    return body


def _swap(quotes: dict, **swap) -> dict:
    body = {"curves": quotes["curves"], "swap": {**quotes["swap"], **swap}}
    r = client.post("/price/rates/swap", json=body)
    assert r.status_code == 200, r.text
    return r.json()


def _swaption_body(quotes: dict, **swaption) -> dict:
    return {
        "curves": quotes["curves"],
        "swaption_vols": quotes["swaption_vols"],
        "swaption": {**quotes["swaption"], **swaption},
    }


def _swaption(quotes: dict, **swaption) -> dict:
    r = client.post("/price/rates/swaption", json=_swaption_body(quotes, **swaption))
    assert r.status_code == 200, r.text
    return r.json()


# ── Curves ───────────────────────────────────────────────────────────────────


def test_the_curves_reprice_their_swaps_and_carry_a_positive_basis(quotes):
    r = client.post("/api/rates/curves", json=quotes["curves"])
    assert r.status_code == 200, r.text
    c = r.json()["curves"]
    assert c["max_repricing_error_bp"] < 1e-6
    # EURIBOR 6M fixes above €STR in the example: two curves, a positive basis.
    assert not c["single_curve"]
    assert all(p["basis_bp"] > 0 for p in c["points"])
    assert "Two curves, not one" in [s["title"] for s in r.json()["methodology"]]


def test_an_overnight_index_gives_one_curve_and_no_basis(quotes):
    """SOFR-style quotes: the index swaps are the OIS swaps, on the same
    frequency. The projection curve is the discount curve."""
    c = quotes["curves"]
    single = {**c, "float_frequency": 1, "deposits": [], "fras": [], "swaps": c["ois"]}
    built = client.post("/api/rates/curves", json=single).json()["curves"]
    assert built["single_curve"]
    assert all(abs(p["basis_bp"]) < 1e-6 for p in built["points"])


def test_quotes_no_curve_can_reprice_are_a_422(quotes):
    bad = {
        **quotes["curves"],
        "swaps": [{"tenor": 2, "rate": 0.02}, {"tenor": 2, "rate": 0.03}],
    }
    for path, body in (
        ("/api/rates/curves", bad),
        ("/price/rates/swap", {"curves": bad, "swap": quotes["swap"]}),
    ):
        r = client.post(path, json=body)
        assert r.status_code == 422
        assert "duplicate maturity" in r.text


# ── Swap ─────────────────────────────────────────────────────────────────────


def test_the_swap_at_its_par_rate_is_worth_nothing(quotes):
    par = _swap(quotes)["swap"]["par_rate"]
    priced = _swap(quotes, fixed_rate=par)
    notional = quotes["swap"]["notional"]
    assert abs(priced["npv"]) < 1e-6 * notional
    assert priced["swap"]["pv01"] == pytest.approx(
        priced["swap"]["annuity"] * notional * 1e-4
    )


def test_the_legs_are_signed_for_the_holder_and_sum_to_the_value(quotes):
    payer = _swap(quotes, fixed_rate=0.02)
    receiver = _swap(quotes, fixed_rate=0.02, payer=False)
    # Paying 2 % against a par rate of 2.73 %: the payer is in the money.
    assert payer["npv"] > 0 and receiver["npv"] == pytest.approx(-payer["npv"])
    for priced, fixed_sign in ((payer, -1), (receiver, 1)):
        s = priced["swap"]
        assert s["fixed_leg"] * fixed_sign > 0 and s["floating_leg"] * fixed_sign < 0
        assert s["fixed_leg"] + s["floating_leg"] == pytest.approx(priced["npv"])
        # Each leg is the sum of its periods' present values.
        for leg in ("fixed", "floating"):
            assert sum(p["present_value"] for p in s[f"{leg}_periods"]) == (
                pytest.approx(s[f"{leg}_leg"])
            )
    # One bp on the fixed rate moves the value by the PV01.
    bumped = _swap(quotes, fixed_rate=0.0201)
    assert payer["npv"] - bumped["npv"] == pytest.approx(payer["swap"]["pv01"])


# ── Swaption ─────────────────────────────────────────────────────────────────


def test_hull_white_is_calibrated_in_bp_of_normal_vol(quotes):
    hw = _swaption(quotes)["swaption"]["hull_white"]
    assert hw["converged"]
    # A two-parameter model on eight quotes: a few bp, not zero, not tens.
    assert 0.0 < hw["rmse_bp"] < 5.0
    for p in hw["points"]:
        assert abs(p["model_vol"] - p["market_vol"]) * 1e4 <= hw["worst_bp"] + 1e-9


def test_every_model_prices_the_same_forward_and_the_bermudan_beats_the_european(
    quotes,
):
    sw = _swaption(quotes)["swaption"]
    models = {p["key"]: p for p in sw["prices"]}
    assert set(models) == {"bachelier", "black", "sabr", "hull_white"}
    for p in sw["prices"]:
        assert p["price"] > 0 and p["implied_normal_vol"] > 0
    assert sw["bermudan_price"] > models["hull_white"]["price"]
    assert sw["switch_premium"] == pytest.approx(
        sw["bermudan_price"] - models["hull_white"]["price"]
    )


def test_the_model_is_chosen_for_the_exercise_and_says_why(quotes):
    european = _swaption(quotes)
    sw = european["swaption"]
    assert (sw["requested"], sw["model"]) == ("auto", "bachelier")
    assert "quote convention" in sw["reason"]
    assert european["npv"] == next(
        p["price"] for p in sw["prices"] if p["key"] == "bachelier"
    )
    # At the money on the quoted vol: nothing the model leaves out.
    assert european["warnings"] == []

    bermudan = _swaption(quotes, exercise="bermudan")
    sw = bermudan["swaption"]
    assert (sw["requested"], sw["model"]) == ("auto", "hull_white")
    assert "several dates" in sw["reason"]
    assert bermudan["npv"] == sw["bermudan_price"]
    assert [w["code"] for w in bermudan["warnings"]] == ["hull_white_fit"]


def test_a_model_chosen_by_hand_is_the_one_priced(quotes):
    for key in ("black", "sabr", "hull_white"):
        priced = _swaption(quotes, model=key)
        sw = priced["swaption"]
        assert (sw["requested"], sw["model"]) == (key, key)
        assert sw["reason"] == "Chosen by hand."
        assert priced["npv"] == next(
            p["price"] for p in sw["prices"] if p["key"] == key
        )


def test_an_atm_vol_on_a_strike_away_from_the_money_is_flagged(quotes):
    forward = _swaption(quotes)["swaption"]["forward"]
    priced = _swaption(quotes, strike=forward + 0.005)
    [warning] = priced["warnings"]
    assert warning["code"] == "atm_vol_off_the_money" and "50 bp" in warning["message"]
    # A vol given by hand is the user's: nothing to flag.
    assert _swaption(quotes, strike=forward + 0.005, normal_vol=0.007)["warnings"] == []


@pytest.mark.parametrize(
    "swaption, message",
    [
        ({"exercise": "bermudan", "model": "sabr"}, "priced under Hull-White"),
        ({"model": "black", "lognormal_vol": None}, "needs a lognormal vol"),
        ({"model": "sabr", "sabr": None}, "needs its parameters"),
        ({"tenor": 4.5}, "whole number of years"),
    ],
)
def test_a_model_without_its_inputs_is_a_422(quotes, swaption, message):
    r = client.post("/price/rates/swaption", json=_swaption_body(quotes, **swaption))
    assert r.status_code == 422
    assert message in r.text


def test_a_fixed_mean_reversion_is_kept(quotes):
    body = {**_swaption_body(quotes), "hull_white_mean_reversion": 0.03}
    hw = client.post("/price/rates/swaption", json=body).json()["swaption"][
        "hull_white"
    ]
    assert hw["mean_reversion"] == pytest.approx(0.03)
    assert hw["mean_reversion_fixed"]


# ── The valuation record ─────────────────────────────────────────────────────


def _record(product_id: str, body: dict) -> Event:
    req = valuation.PRODUCTS[product_id].request.model_validate(body)
    priced = valuation.price(product_id, req)
    return Event.create(
        "pricing.valuation", valuation.payload(product_id, req, priced, ip_hash=None)
    )


def test_a_swap_valuation_is_recorded_and_replays(quotes):
    event = _record(
        "interest_rate_swap", {"curves": quotes["curves"], "swap": quotes["swap"]}
    )
    p = event.payload
    assert p["model"]["name"] == "multi_curve" and p["engine"]["name"] == "analytic"
    # Priced on the request's quotes: the store is not read.
    assert p["market_inputs"] == []
    result = replay.replay(event.model_dump(mode="json"))
    assert result.outcome == replay.ReplayOutcome.REPRODUCED
    assert result.npv_difference == 0.0


@pytest.mark.parametrize(
    "exercise, model, engine",
    [("european", "bachelier", "analytic"), ("bermudan", "hull_white", "lattice")],
)
def test_a_swaption_valuation_records_the_model_it_ran(quotes, exercise, model, engine):
    event = _record("swaption", _swaption_body(quotes, exercise=exercise))
    p = event.payload
    assert (p["model"]["name"], p["engine"]["name"]) == (model, engine)
    assert p["model"]["params"]["requested"] == "auto"
    assert p["model"]["params"]["hull_white_sigma"] > 0
    result = replay.replay(event.model_dump(mode="json"))
    assert result.outcome == replay.ReplayOutcome.REPRODUCED
    assert result.npv_difference == 0.0


# ── Quote sets ───────────────────────────────────────────────────────────────


def test_each_quote_set_says_where_its_numbers_come_from():
    eur = " ".join(
        p
        for s in rates_derivatives.quotes_methodology("eur-illustrative")
        for p in s.paragraphs
    )
    assert "illustrative EUR quotes, which are not market data" in eur
    usd = " ".join(
        p
        for s in rates_derivatives.quotes_methodology("usd-sofr")
        for p in s.paragraphs
    )
    # The USD set is trades, not quotes, and says what it leaves out.
    assert "not quotes but trades" in usd and "read the trade counts" in usd


def test_the_example_is_a_valid_quote_set_and_an_unknown_set_is_refused():
    RatesQuoteSetResponse.model_validate(
        rates_derivatives.quote_set("eur-illustrative")
    )
    assert client.get("/api/rates/quotes/gbp-sonia").status_code == 422
