"""Rates page — `rates.py` and `/market/rates/*`.

Property tests on the real C++ bootstrap (the wheel), the database replaced by
a fake: every par quote is repriced to par by the curve built from it, a
published zero curve round-trips, forwards and zeros agree on a flat curve,
the JGB first-pillar bias matches its closed form, and the API never draws a
term structure it cannot defend (GBP: quoted only; CHF: none).
"""

from __future__ import annotations

import math
import os
from datetime import date, timedelta

os.environ.setdefault("JWT_SECRET", "test-only-secret-not-for-deployment")

import pytest
from fastapi.testclient import TestClient

import quantmodeling as qm
from api.app import db, rates
from api.app.main import app
from api.app.routers import market_rates

TODAY = date.today()

# Percent, as stored by data-ingest / FRED.
USD_PCT = {
    "DGS1MO": 3.97,
    "DGS3MO": 4.14,
    "DGS6MO": 4.24,
    "DGS1": 4.44,
    "DGS2": 4.76,
    "DGS3": 4.83,
    "DGS5": 4.86,
    "DGS7": 4.93,
    "DGS10": 5.01,
    "DGS20": 5.38,
    "DGS30": 5.34,
}
JGB_PCT = {
    f"JPY.JGB_{t}Y": v
    for t, v in zip(
        (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 15, 20, 25, 30, 40),
        (
            1.578,
            1.849,
            1.982,
            2.16,
            2.305,
            2.423,
            2.538,
            2.7,
            2.838,
            2.981,
            3.519,
            3.812,
            4.072,
            4.044,
            4.033,
        ),
    )
}
EUR_PCT = {
    f"EUR.AAA_SPOT_{t}": v
    for t, v in zip(
        ("3M", "6M", "1Y", "2Y", "3Y", "5Y", "7Y", "10Y", "15Y", "20Y", "30Y"),
        (2.641, 2.803, 3.022, 3.218, 3.28, 3.33, 3.396, 3.524, 3.699, 3.787, 3.763),
    )
}
GILT_PCT = {
    "GBP.GILT_PAR_5Y": 4.78,
    "GBP.GILT_PAR_10Y": 5.191,
    "GBP.GILT_PAR_20Y": 5.545,
}


@pytest.fixture
def store(monkeypatch):
    """A fake rates store: every curve complete on `as_of`."""
    state = {
        "as_of": TODAY - timedelta(days=1),
        "curves": {**USD_PCT, **JGB_PCT, **EUR_PCT, **GILT_PCT},
    }

    def snapshot(table, ids):
        if not all(i in state["curves"] for i in ids):
            return None
        return state["as_of"], {i: state["curves"][i] for i in ids}

    def latest(table, ids):
        return {i: (state["as_of"], 2.5) for i in ids}

    def history(table, sid, since):
        return [(since + timedelta(days=k), 2.0 + k / 100) for k in range(3)]

    monkeypatch.setattr(db, "rates_curve_snapshot", snapshot)
    monkeypatch.setattr(db, "rates_latest", latest)
    monkeypatch.setattr(db, "rates_history", history)
    return state


def _curve(currency):
    spec = rates.CATALOG[currency].government
    quoted = {k: v / 100 for k, v in {**USD_PCT, **JGB_PCT, **EUR_PCT}.items()}
    return spec, quoted, rates.discount_curve(spec, quoted)


# ── The bootstrap reprices its inputs ────────────────────────────────────────


@pytest.mark.parametrize("currency", ["USD", "JPY"])
def test_every_par_quote_is_repriced_to_par(currency):
    spec, quoted, (times, dfs) = _curve(currency)
    for p in spec.pillars:
        y = quoted[p.series_id]
        coupons = [0.5 * k for k in range(1, int(round(p.tenor / 0.5)) + 1)]
        df = qm.discount_factors(times, dfs, coupons)
        price = y / 2 * sum(df) + df[-1]
        assert price == pytest.approx(1.0, abs=1e-9), p.label


def test_every_bill_deposit_is_repriced():
    spec, quoted, (times, dfs) = _curve("USD")
    for p in spec.money_market:
        [df] = qm.discount_factors(times, dfs, [p.tenor])
        assert df == pytest.approx(1 / (1 + quoted[p.series_id] * p.tenor), rel=1e-12)


def test_a_published_zero_curve_round_trips_at_its_pillars():
    spec, quoted, (times, dfs) = _curve("EUR")
    zero, _ = rates.derived_curves(times, dfs, 0.5)
    at = {round(p.tenor, 9): p.rate for p in zero}
    for p in spec.pillars:
        assert at[round(p.tenor, 9)] == pytest.approx(quoted[p.series_id], abs=1e-12)


def test_on_a_flat_curve_forward_equals_zero_everywhere():
    times = [0.5, 1, 2, 5, 10, 30]
    dfs = [math.exp(-0.03 * t) for t in times]
    zero, forward = rates.derived_curves(times, dfs, 1.0)
    assert all(p.rate == pytest.approx(0.03, abs=1e-12) for p in zero + forward)


def test_derived_curves_stay_between_the_first_and_last_pillar():
    spec, quoted, (times, dfs) = _curve("EUR")
    zero, forward = rates.derived_curves(times, dfs, 2.0)
    assert min(p.tenor for p in zero) == pytest.approx(0.25)
    assert max(p.tenor for p in zero) == pytest.approx(30.0)
    assert max(p.tenor + 2.0 for p in forward) <= 30.0 + 1e-9


def test_jgb_first_pillar_bias_matches_its_closed_form():
    """Flat-before-the-first-pillar gives z = ln(1+y); log-linear from the
    origin would give 2 ln(1+y/2); the methodology states the gap."""
    spec, quoted, (times, dfs) = _curve("JPY")
    y = quoted["JPY.JGB_1Y"]
    z_boot = -math.log(dfs[0]) / times[0]
    z_origin = 2 * math.log(1 + y / 2)
    assert (z_origin - z_boot) * 1e4 == pytest.approx(
        rates.first_pillar_bias_bp(y), rel=1e-9
    )
    assert rates.first_pillar_bias_bp(y) < 1.0  # at today's ~1.6 %; ≈ y²/4


# ── The endpoint ─────────────────────────────────────────────────────────────


@pytest.fixture
def client(store):
    market_rates._OVERVIEW_CACHE = type(market_rates._OVERVIEW_CACHE)(
        max_size=40, ttl_seconds=3600
    )
    market_rates._HISTORY_CACHE = type(market_rates._HISTORY_CACHE)(
        max_size=60, ttl_seconds=3600
    )
    return TestClient(app)


def test_rates_are_decimals_not_percent(client):
    body = client.get("/market/rates/overview?currency=USD").json()
    assert body["unit"] == "decimal"
    assert all(0 < p["rate"] < 0.2 for p in body["government"]["quoted"])
    assert all(0 < p["rate"] < 0.2 for p in body["government"]["zero"])
    assert all(b["rate"] == pytest.approx(0.025) for b in body["benchmarks"])


def test_gbp_is_quoted_only_and_says_why(client):
    g = client.get("/market/rates/overview?currency=GBP").json()["government"]
    assert g["zero"] is None and g["forward"] is None
    assert "three par yields" in g["no_derivation"]
    assert [p["label"] for p in g["quoted"]] == ["5Y", "10Y", "20Y"]


def test_chf_has_no_government_curve_and_says_why(client):
    body = client.get("/market/rates/overview?currency=CHF").json()
    assert body["government"] is None
    assert "stopped publishing" in body["government_unavailable"]
    assert body["headline_series"] == "CHF.SARON"


def test_averages_are_flagged_backward_looking(client):
    bench = client.get("/market/rates/overview?currency=USD").json()["benchmarks"]
    flags = {b["series_id"]: b["backward_looking"] for b in bench}
    assert flags["SOFR"] is False and flags["SOFR90DAYAVG"] is True


def test_every_currency_ships_its_methodology(client):
    for ccy in rates.CURRENCIES:
        titles = [
            s["title"]
            for s in client.get(f"/market/rates/overview?currency={ccy}").json()[
                "methodology"
            ]
        ]
        assert (
            "Derived zero and forward curves" in titles and f"{ccy} specifics" in titles
        )


def test_jpy_methodology_states_todays_bias(client):
    body = client.get("/market/rates/overview?currency=JPY").json()
    text = " ".join(p for s in body["methodology"] for p in s["paragraphs"])
    assert "0.61 bp" in text and "1.578 %" in text


def test_an_incomplete_curve_is_reported_not_spliced(client, store):
    del store["curves"]["DGS7"]
    body = client.get("/market/rates/overview?currency=USD").json()
    assert body["government"] is None
    assert "no date on which every pillar" in body["government_unavailable"]


def test_a_stale_curve_is_flagged(client, store):
    store["as_of"] = TODAY - timedelta(days=30)
    assert any(
        "30 days ago" in w
        for w in client.get("/market/rates/overview?currency=EUR").json()["warnings"]
    )


def test_history_only_serves_the_currencys_reference_rates(client):
    ok = client.get("/market/rates/history?currency=EUR&series_id=EUR.ESTR&years=1")
    assert ok.status_code == 200 and len(ok.json()["points"]) == 3
    assert ok.json()["points"][0]["rate"] == pytest.approx(0.02)
    assert (
        client.get("/market/rates/history?currency=EUR&series_id=SOFR").status_code
        == 404
    )
    assert (
        client.get("/market/rates/history?currency=EUR&series_id=DGS10").status_code
        == 404
    )
