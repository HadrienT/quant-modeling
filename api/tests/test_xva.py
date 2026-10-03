"""xVA of a netting set — `xva.py` and `/api/xva/*`, on the real C++ library
and a fake store.

The properties are those of blueprint/wp/23-xva.md §15, seen through the API:
signs, a bought option's constant exposure, a sold option's absence of
exposure, netting and the shares of each trade, what a CSA is worth, the two
measures, determinism, explicit errors and the audit record.
"""

from __future__ import annotations

import math
import os
from datetime import date, timedelta

os.environ.setdefault("JWT_SECRET", "test-only-secret-not-for-deployment")

import pandas as pd
import pytest
from fastapi.testclient import TestClient

from api.app import credit, db, rates_derivatives, xva
from api.app.main import app
from api.app.routers import xva as xva_router

client = TestClient(app)

AS_OF = date(2026, 9, 30)
PAR = {
    1: 0.0455,
    2: 0.0473,
    3: 0.0477,
    5: 0.0478,
    7: 0.0481,
    10: 0.0486,
    15: 0.0500,
    20: 0.0503,
    30: 0.0494,
}
VOLS = [
    (0.5, 10, 0.0100),
    (1, 2, 0.0117),
    (1, 5, 0.0111),
    (1, 10, 0.0103),
    (2, 5, 0.0114),
    (2, 10, 0.0107),
]
#: ICE BofA OAS by rating, percent.
SPREADS_PCT = {
    "AAA": 0.38,
    "AA": 0.52,
    "A": 0.71,
    "BBB": 1.02,
    "BB": 1.85,
    "B": 2.95,
    "CCC and lower": 7.9,
}
#: S&P's yearly default rates by category, percent, with the defaults of the
#: year: three of the years CEREP holds (2008, 2009, 2024), repeated to make a
#: history.
DEFAULT_RATES = {
    "AAA": [(0, 0.0), (0, 0.0), (0, 0.0)],
    "AA": [(2, 0.24), (0, 0.0), (0, 0.0)],
    "A": [(6, 0.34), (0, 0.0), (0, 0.0)],
    "BBB": [(5, 0.32), (11, 0.69), (0, 0.0)],
    "BB": [(8, 0.71), (7, 0.78), (2, 0.21)],
    "B": [(46, 3.95), (124, 11.7), (30, 1.88)],
    "CCC": [(24, 27.59), (70, 45.64), (92, 28.52)],
}
PATHS = 2000


def _history() -> pd.Series:
    """Ten years of a mean-reverting daily rate around 2.5 %, in percent."""
    values, r = [], 4.0
    for k in range(2600):
        shock = math.sin(12.9898 * k) * 43758.5453
        r += 0.0012 * (2.5 - r) + 0.05 * (2.0 * (shock - math.floor(shock)) - 1.0)
        values.append(r)
    index = pd.to_datetime([date(2016, 1, 4) + timedelta(days=k) for k in range(2600)])
    return pd.Series(values, index=index)


@pytest.fixture(autouse=True)
def store(monkeypatch):
    state = {"market": True}

    def market_request(today=None):
        if not state["market"]:
            raise rates_derivatives.RatesMarketUnavailable(
                "No SOFR swap curve in the store"
            )
        quotes = [{"tenor": float(t), "rate": r} for t, r in PAR.items()]
        return {
            "as_of": AS_OF,
            "label": "USD SOFR, from traded prices",
            "ois": quotes,
            "swaption_vols": [
                {"expiry": e, "tenor": float(t), "normal_vol": v} for e, t, v in VOLS
            ],
            "trades_used": 180,
        }

    monkeypatch.setattr(rates_derivatives, "market_request", market_request)
    by_series = {r.series_id: SPREADS_PCT[r.label] for r in credit.RATINGS}
    monkeypatch.setattr(
        db,
        "rates_curve_snapshot",
        lambda table, ids: (date.today(), {i: by_series[i] for i in ids}),
    )
    monkeypatch.setattr(db, "fred_series", lambda series_id, since=None: _history())

    def default_rates(agency, rating):
        if not state["default_rates"]:
            return []
        assert agency == "S&P"
        return [
            (date(2000 + k, 1, 1), *DEFAULT_RATES[rating][k % 3]) for k in range(12)
        ]

    state["default_rates"] = True
    monkeypatch.setattr(db, "rating_default_rates", default_rates)
    events = []
    monkeypatch.setattr(
        xva_router,
        "emit",
        lambda event_type, payload: events.append((event_type, payload)),
    )
    state["events"] = events
    return state


def run(**body) -> dict:
    r = client.post("/api/xva/netting-set", json={"paths": PATHS, **body})
    assert r.status_code == 200, r.text
    return r.json()


def test_the_portfolios_and_ratings_are_listed():
    body = client.get("/api/xva/portfolios").json()
    assert [p["id"] for p in body["portfolios"]] == list(xva.PORTFOLIOS)
    assert all(p["lesson"] for p in body["portfolios"])
    assert body["ratings"] == ["AAA", "AA", "A", "BBB", "BB", "B", "CCC"]
    # The wrong-way parameter is offered as scenarios, each with what it
    # assumes; the figures of the text are those of the parameter.
    scenarios = {s["id"]: s for s in body["wrong_way_scenarios"]}
    assert list(scenarios) == [
        "independent",
        "wrong_way",
        "strong_wrong_way",
        "right_way",
    ]
    assert scenarios["independent"]["wrong_way_risk"] == 0.0
    assert "rises by 65 %" in scenarios["wrong_way"]["explanation"]  # e^0.5
    assert "multiplied by 2.7" in scenarios["strong_wrong_way"]["explanation"]  # e^1
    assert "falls by 39 %" in scenarios["right_way"]["explanation"]  # e^-0.5
    assert all(s["explanation"] for s in scenarios.values())
    # Each one is a value the endpoint takes.
    bounds = xva_router.XvaRequest.model_fields["wrong_way_risk"].metadata
    assert all(-30 <= s["wrong_way_risk"] <= 30 for s in scenarios.values()) and bounds


def test_a_swap_at_par_costs_a_cva_and_earns_a_dva():
    body = run(portfolio="single_swap")
    (trade,) = body["trades"]
    assert (
        abs(trade["value_today"]) < 1e-6 * trade["notional"]
    )  # struck at the par rate
    adj = body["adjustments"]
    # Cash-flow convention: a cost is negative.
    assert adj["cva"]["value"] < 0 < adj["dva"]["value"]
    assert 0 < adj["cva"]["error"] < 0.1 * abs(adj["cva"]["value"])
    # First to default removes scenarios; the rule of thumb gives the order.
    assert adj["cva_unilateral"] < adj["cva"]["value"]
    assert adj["cva_rule_of_thumb"] / adj["cva"]["value"] == pytest.approx(
        1.0, abs=0.25
    )
    # Alone, stand-alone, incremental and marginal CVA are the same number.
    assert trade["standalone_cva"] == pytest.approx(adj["cva"]["value"], rel=1e-9)
    assert trade["incremental_cva"] == pytest.approx(adj["cva"]["value"], rel=1e-9)
    assert trade["marginal_cva"] == pytest.approx(adj["cva"]["value"], rel=1e-9)
    # The profile is a hump that ends at zero.
    ee = body["exposure"]["ee"]
    assert ee[-1] == 0.0 and max(ee) > 10 * ee[0] > 0
    # The distribution behind the profile: quantiles of the value, increasing
    # in their level, the PFE being the positive part of its own.
    exposure = body["exposure"]
    levels, quantiles = exposure["quantile_levels"], exposure["value_quantiles"]
    assert levels == sorted(levels) and 0.95 in levels
    assert all(len(q) == len(exposure["times"]) for q in quantiles)
    assert all(
        low[i] <= high[i]
        for low, high in zip(quantiles, quantiles[1:])
        for i in range(len(low))
    )
    assert exposure["pfe"] == pytest.approx(
        [max(v, 0.0) for v in quantiles[levels.index(0.95)]]
    )


def test_what_the_computation_rests_on_is_returned():
    m = run()["market"]
    assert m["curve_as_of"] == AS_OF.isoformat() and m["currency"] == "USD"
    hw = m["hull_white"]
    assert hw["vol_points"] == len(VOLS) and hw["swaption_trades"] == 180
    assert 0.008 < hw["sigma"] < 0.016 and hw["rmse_bp"] < 5
    cp = m["counterparty"]
    assert cp["rating"] == "BBB" and cp["spread"] == pytest.approx(0.0102)
    # Credit triangle: hazard ≈ spread / (1 − recovery).
    assert cp["hazard"] == pytest.approx(0.0102 / 0.6, rel=0.05)
    assert m["own"]["rating"] == "A"
    h = m["historical"]
    assert (
        h["series"] == "DGS3MO" and h["observations"] == 2600 and h["overridden"] == []
    )
    assert h["sigma"] == h["estimated_sigma"] > 0


def test_a_riskier_counterparty_costs_more_and_a_safer_bank_earns_less():
    cva = [
        run(counterparty_rating=r)["adjustments"]["cva"]["value"]
        for r in ("AA", "BBB", "B", "CCC")
    ]
    assert cva == sorted(cva, reverse=True) and cva[-1] < 3 * cva[1] < 0
    risky_bank = run(own_rating="BB")["adjustments"]["dva"]["value"]
    safe_bank = run(own_rating="AAA")["adjustments"]["dva"]["value"]
    assert risky_bank > safe_bank > 0


def test_a_bought_option_is_never_a_liability_until_it_expires():
    body = run(portfolio="bought_swaption", paths=10000)
    e = body["exposure"]
    v0 = body["value_today"]
    assert v0 > 0
    for t, ee, ene, err in zip(
        e["times"], e["discounted_ee"], e["discounted_ene"], e["discounted_ee_error"]
    ):
        if t <= 1.0 + 1e-9:
            assert ene == 0.0
            assert ee == pytest.approx(v0, abs=4 * err)  # EE* = V0: a martingale


def test_a_sold_option_has_no_exposure_until_it_expires():
    body = run(portfolio="sold_swaption")
    e = body["exposure"]
    assert body["value_today"] < 0
    assert all(ee == 0.0 for t, ee in zip(e["times"], e["ee"]) if t <= 1.0 + 1e-9)
    assert body["adjustments"]["dva"]["value"] > 0


def test_netting_and_the_share_of_each_trade():
    directional = run(portfolio="directional")
    balanced = run(portfolio="balanced")

    def netting_ratio(body):
        return body["adjustments"]["cva"]["value"] / sum(
            t["standalone_cva"] for t in body["trades"]
        )

    # Trades that move together barely net; trades that offset do.
    assert netting_ratio(directional) > 0.9
    assert netting_ratio(balanced) < 0.5
    # Euler shares add up to the CVA of the set.
    for body in (directional, balanced):
        assert sum(t["marginal_cva"] for t in body["trades"]) == pytest.approx(
            body["adjustments"]["cva"]["value"], rel=1e-9
        )
    # In the directional book every trade costs; in the balanced one a hedge
    # has a positive incremental CVA.
    assert all(t["incremental_cva"] < 0 for t in directional["trades"])
    assert any(t["incremental_cva"] > 0 for t in balanced["trades"])
    assert all(
        t["incremental_cva"] >= t["standalone_cva"] - 1e-6 for t in balanced["trades"]
    )


def test_a_csa_shrinks_exposure_and_cva_on_the_same_paths():
    body = run(portfolio="single_swap", csa={})
    open_cva = body["adjustments_uncollateralised"]["cva"]["value"]
    assert open_cva < 3 * body["adjustments"]["cva"]["value"] < 0
    assert max(body["exposure"]["pfe"]) < 0.5 * max(
        body["exposure_uncollateralised"]["pfe"]
    )
    # No exact allocation under a CSA.
    assert body["trades"][0]["marginal_cva"] is None
    # A threshold lets exposure build up again.
    with_threshold = run(
        portfolio="single_swap", csa={"threshold_counterparty": 200_000}
    )
    assert (
        with_threshold["adjustments"]["cva"]["value"]
        < body["adjustments"]["cva"]["value"]
    )
    # Without a CSA there is nothing to compare with.
    plain = run(portfolio="single_swap")
    assert (
        plain["exposure_uncollateralised"] is None
        and plain["adjustments_uncollateralised"] is None
    )


def test_risk_measures_follow_the_historical_dynamics_the_user_sets():
    low = run(historical={"long_run_rate": 0.02, "mean_reversion": 0.3})
    high = run(historical={"long_run_rate": 0.07, "mean_reversion": 0.3})
    # A payer swap gains when rates revert to a level above the forwards.
    assert high["risk"]["epe"] > 2 * low["risk"]["epe"]
    assert set(high["market"]["historical"]["overridden"]) == {
        "long_run_rate",
        "mean_reversion",
    }
    assert high["market"]["historical"]["long_run_rate"] == 0.07
    # The estimate is still reported next to what replaced it.
    assert high["market"]["historical"]["estimated_long_run_rate"] != 0.07
    # Prices do not depend on the historical measure.
    assert high["adjustments"]["cva"] == low["adjustments"]["cva"]
    calm = run(
        historical={"sigma": 0.004, "long_run_rate": 0.045, "mean_reversion": 0.3}
    )
    wild = run(
        historical={"sigma": 0.012, "long_run_rate": 0.045, "mean_reversion": 0.3}
    )
    assert max(wild["risk"]["pfe"]) > 2 * max(calm["risk"]["pfe"])


def test_the_seed_makes_the_run_reproducible():
    a, b = run(seed=7), run(seed=7)
    assert a["adjustments"] == b["adjustments"] and a["exposure"] == b["exposure"]
    c = run(seed=8)
    assert c["adjustments"]["cva"]["value"] != a["adjustments"]["cva"]["value"]
    errors = math.hypot(
        a["adjustments"]["cva"]["error"], c["adjustments"]["cva"]["error"]
    )
    assert (
        abs(a["adjustments"]["cva"]["value"] - c["adjustments"]["cva"]["value"])
        < 4 * errors
    )


def test_the_page_explains_itself_and_says_what_is_a_proxy():
    body = run()
    titles = [s["title"] for s in body["methodology"]]
    assert titles[0] == "What is computed" and titles[-1] == "Sources"
    text = " ".join(p for s in body["methodology"] for p in s["paragraphs"])
    assert (
        "Gregory" in text
        and "negative when it is a cost" in text
        and "first default" in text
    )
    assert any("rating proxies" in w for w in body["warnings"])
    assert any("change with the window" in w for w in body["warnings"])
    assert body["lesson"] and body["portfolio_label"]


def test_missing_inputs_are_explicit_errors_not_fallbacks(store, monkeypatch):
    store["market"] = False
    r = client.post("/api/xva/netting-set", json={"paths": PATHS})
    assert r.status_code == 503 and "No SOFR swap curve" in r.json()["message"]
    store["market"] = True

    monkeypatch.setattr(db, "rates_curve_snapshot", lambda table, ids: None)
    r = client.post("/api/xva/netting-set", json={"paths": PATHS})
    assert r.status_code == 503 and "rating spread" in r.json()["message"]
    monkeypatch.undo()


def test_without_default_rates_the_capital_needs_a_pd(store):
    store["default_rates"] = False
    r = client.post("/api/xva/netting-set", json={"paths": PATHS})
    assert r.status_code == 503 and "rating-default-rates" in r.text
    # Nothing is read when the request brings its own.
    assert run(capital={"pd": 0.01})["capital"]["pd_source"] == "entered"


def test_no_history_or_an_unreachable_store(monkeypatch):
    monkeypatch.setattr(
        db, "fred_series", lambda series_id, since=None: pd.Series(dtype="float64")
    )
    r = client.post("/api/xva/netting-set", json={"paths": PATHS})
    assert r.status_code == 503 and "history of DGS3MO" in r.json()["message"]

    def down(*_, **__):
        raise db.StoreUnavailable("connection refused")

    monkeypatch.setattr(db, "fred_series", down)
    r = client.post("/api/xva/netting-set", json={"paths": PATHS})
    assert (
        r.status_code == 503 and r.json()["message"] == "Market data store unavailable"
    )


def test_a_trending_history_needs_the_user_to_set_the_reversion(monkeypatch):
    trend = pd.Series(
        [
            1.0005**k + 0.0001 * math.sin(k) for k in range(2600)
        ],  # ever faster: no level to revert to
        index=pd.to_datetime(
            [date(2016, 1, 4) + timedelta(days=k) for k in range(2600)]
        ),
    )
    monkeypatch.setattr(db, "fred_series", lambda series_id, since=None: trend)
    r = client.post("/api/xva/netting-set", json={"paths": PATHS})
    assert r.status_code == 503 and "set the mean reversion" in r.json()["message"]
    body = run(historical={"mean_reversion": 0.2, "long_run_rate": 0.03})
    h = body["market"]["historical"]
    assert (
        h["estimated_mean_reversion"] is None
        and h["mean_reversion"] == 0.2
        and h["sigma"] > 0
    )


def test_invalid_requests_are_rejected():
    assert (
        client.post(
            "/api/xva/netting-set", json={"counterparty_rating": "ZZZ"}
        ).status_code
        == 422
    )
    assert (
        client.post(
            "/api/xva/netting-set", json={"portfolio": "everything"}
        ).status_code
        == 422
    )
    assert client.post("/api/xva/netting-set", json={"paths": 10}).status_code == 422
    assert (
        client.post("/api/xva/netting-set", json={"recovery": 1.0}).status_code == 422
    )


def test_every_run_leaves_an_audit_record_that_can_be_run_again(store):
    body = run(portfolio="balanced", seed=11, counterparty_rating="BB")
    ((event_type, payload),) = store["events"]
    assert event_type == "xva.valuation"
    assert payload.request["portfolio"] == "balanced" and payload.request["seed"] == 11
    assert payload.request_hash.startswith("sha256:")
    assert payload.engine.seed == 11 and payload.engine.n_paths == PATHS
    assert payload.model.params["sigma"] == body["market"]["hull_white"]["sigma"]
    names = [m.name for m in payload.market_inputs]
    assert names == [
        "sofr_swap_curve",
        "hull_white_calibration",
        "credit_spread:BB",
        "credit_spread:A",
        "rate_history:DGS3MO",
        "default_rate:BB",
    ]
    assert all(m.value_hash.startswith("sha256:") for m in payload.market_inputs)
    assert payload.result.cva == body["adjustments"]["cva"]["value"]
    # Nothing but the request, the model and hashes: no credential can be here.
    assert set(payload.request) == set(xva_router.XvaRequest.model_fields)
    # A failed run emits nothing.
    store["events"].clear()
    store["market"] = False
    client.post("/api/xva/netting-set", json={"paths": PATHS})
    assert store["events"] == []


# ── Lot X4: a Bermudan, valued by regression ─────────────────────────────────


def test_a_bermudan_is_valued_by_regression_and_is_an_asset_until_exercised():
    body = run(portfolio="bermudan")
    (trade,) = body["trades"]
    assert trade["kind"] == "bermudan" and trade["start"] == 1.0
    assert trade["value_today"] > 0  # the lattice price
    # Fitted on independent pilot paths; a swap needs none.
    assert body["pilot_paths"] >= 20_000
    assert run(portfolio="single_swap")["pilot_paths"] == 0

    e = body["exposure"]
    before = [i for i, t in enumerate(e["times"]) if t < 1.0 - 1e-9]
    assert before
    for i in before:
        # A bought option: nothing is owed, and its discounted expected
        # exposure is its price — here the price under the fitted rule, a few
        # percent at most under the lattice's.
        assert e["discounted_ene"][i] == 0.0
        assert e["discounted_ee"][i] == pytest.approx(trade["value_today"], rel=0.06)
    # Once exercised it is a swap, which can turn against the bank.
    assert min(e["discounted_ene"]) < 0.0
    adj = body["adjustments"]
    assert adj["cva"]["value"] < 0 < adj["dva"]["value"]
    # The risk measures run under the historical measure, regression included.
    assert max(body["risk"]["pfe"]) > 0
    assert "Bermudans: value by regression" in [s["title"] for s in body["methodology"]]


def test_the_right_to_cancel_a_swap_removes_what_the_bank_would_owe():
    swap = run(portfolio="single_swap")
    cancellable = run(portfolio="cancellable")
    payer, right = cancellable["trades"]
    assert (payer["kind"], right["kind"]) == ("swap", "bermudan")
    # The right is struck at the swap's own fixed rate, and it is a receiver:
    # exercising it cancels the payer swap.
    assert right["fixed_rate"] == pytest.approx(payer["fixed_rate"])
    assert not right["payer"]
    assert cancellable["value_today"] == pytest.approx(right["value_today"], rel=1e-6)

    # The bank cancels when the swap has turned against it: less to owe...
    assert (
        cancellable["adjustments"]["dva"]["value"]
        < 0.6 * swap["adjustments"]["dva"]["value"]
    )
    # ...while what it is owed is still there, plus the option's own value.
    assert (
        cancellable["adjustments"]["cva"]["value"] < swap["adjustments"]["cva"]["value"]
    )
    assert max(cancellable["exposure"]["ee"]) > max(swap["exposure"]["ee"])


# ── Lot X5: margin and capital ───────────────────────────────────────────────


def test_initial_margin_turns_the_cva_into_an_mva():
    csa = {"cashflows": "withheld"}
    vm = run(portfolio="single_swap", csa=csa, borrowing_spread=0.005)
    im = run(
        portfolio="single_swap",
        csa={**csa, "initial_margin": True, "initial_margin_model": "regression"},
        borrowing_spread=0.005,
    )
    assert im["initial_margin"]["model"] == "regression"
    assert im["initial_margin"]["simm_today"] is None
    assert vm["initial_margin"] is None and vm["adjustments"]["mva"] == 0.0
    margin = im["initial_margin"]
    assert margin["today"] > 0
    assert (
        len(margin["expected"]) == len(margin["times"]) == len(im["exposure"]["times"])
    )
    # A 10-year swap of 10 M: ten days of rate moves at 99 % is a few
    # hundred thousand.
    assert 50_000 < margin["today"] < 1_000_000
    # The margin covers 99 % of the move: almost no CVA left...
    assert abs(im["adjustments"]["cva"]["value"]) < 0.1 * abs(
        vm["adjustments"]["cva"]["value"]
    )
    # ...and a cost of funding the margin posted, larger than the CVA removed.
    removed = im["adjustments"]["cva"]["value"] - vm["adjustments"]["cva"]["value"]
    assert im["adjustments"]["mva"] < -removed < 0
    # Segregated margin funds nothing.
    assert im["adjustments"]["fca"] == pytest.approx(vm["adjustments"]["fca"])

    # Anchored on today's actual margin, the profile and the MVA scale.
    anchored = run(
        portfolio="single_swap",
        csa={
            **csa,
            "initial_margin": True,
            "initial_margin_today": 2 * margin["today"],
        },
        borrowing_spread=0.005,
    )
    assert anchored["initial_margin"]["today"] == pytest.approx(2 * margin["today"])
    assert anchored["adjustments"]["mva"] == pytest.approx(2 * im["adjustments"]["mva"])
    assert "Initial margin and MVA" in [s["title"] for s in im["methodology"]]


def test_colva_is_the_rate_paid_on_the_collateral():
    flat = run(portfolio="bought_swaption", csa={})
    assert flat["adjustments"]["colva"] == 0.0
    # The bank holds collateral against the option it bought; the agreement
    # pays 25 bp more than the discount rate on it: a cost.
    costly = run(portfolio="bought_swaption", csa={"collateral_rate_spread": 0.0025})
    cheap = run(portfolio="bought_swaption", csa={"collateral_rate_spread": -0.0025})
    assert costly["adjustments"]["colva"] < 0
    assert cheap["adjustments"]["colva"] == pytest.approx(
        -costly["adjustments"]["colva"]
    )
    assert run(portfolio="bought_swaption")["adjustments"]["colva"] == 0.0


def test_capital_and_its_cost():
    body = run(portfolio="single_swap")
    k = body["capital"]
    assert k["ead_today"] > 0 and not k["margined"]
    assert k["default_capital_today"] > 0 and k["cva_capital_today"] > 0
    assert k["expected_ead"][-1] == 0.0  # nothing left at maturity
    assert body["adjustments"]["kva"] < 0
    # No PD given: the historical default rate of the rating, with what it
    # rests on -- far below what the spread implies.
    assert k["pd_source"] == "historical"
    basis = body["market"]["default_rate"]
    assert (basis["agency"], basis["rating"]) == ("S&P", "BBB")
    assert (basis["first_year"], basis["last_year"], basis["years"]) == (2000, 2011, 12)
    assert basis["defaults"] == 4 * (5 + 11)
    assert basis["rate"] == pytest.approx((0.32 + 0.69 + 0.0) / 3 / 100)
    assert k["pd"] == pytest.approx(basis["rate"])
    assert k["pd"] < 0.25 * body["market"]["counterparty"]["hazard"]
    assert "S&P's BBB corporate ratings from 2000 to 2011" in k["pd_reason"]
    assert "0.34 % (64 defaults in 12 years" in k["pd_reason"]
    assert (k["lgd"], k["sector"], k["investment_grade"]) == (0.40, "other", True)

    # A bank's own PD, higher: more default capital, and nothing read.
    own = run(portfolio="single_swap", capital={"pd": 0.02})
    assert own["capital"]["pd_source"] == "entered"
    assert own["market"]["default_rate"] is None
    assert own["capital"]["default_capital_today"] > k["default_capital_today"]
    assert own["capital"]["cva_capital_today"] == pytest.approx(k["cva_capital_today"])
    # A rating that never defaulted on record: the regulatory floor.
    safe = run(portfolio="single_swap", counterparty_rating="AAA")["capital"]
    assert safe["pd"] == 0.0005 and "floor" in safe["pd_reason"]
    # A financial counterparty: 45 % LGD (CRE32.6).
    assert run(capital={"sector": "financial"})["capital"]["lgd"] == 0.45
    # KVA is linear in the cost of capital.
    dear = run(portfolio="single_swap", capital={"cost_of_capital": 0.15})
    assert dear["adjustments"]["kva"] == pytest.approx(1.5 * body["adjustments"]["kva"])

    # Under a CSA the netting set is margined: less capital, and the same
    # paths without the CSA are reported next to it.
    margined = run(portfolio="single_swap", csa={})
    assert margined["capital"]["margined"]
    assert margined["capital"]["ead_today"] < k["ead_today"]
    assert margined["capital_uncollateralised"]["ead_today"] == pytest.approx(
        k["ead_today"]
    )
    assert margined["adjustments"]["kva"] > body["adjustments"]["kva"]
    assert "Capital and KVA" in [s["title"] for s in body["methodology"]]


# ── Lot X4b: a trade written as a script ─────────────────────────────────────


def test_a_scripted_trade_has_an_exposure_in_the_library():
    """The binding takes a rates-only payoff script as a trade: a zero-coupon
    bond bought is an asset worth its discount factor until it pays."""
    import quantmodeling as qm

    times = [1.0, 2.0, 5.0, 10.0]
    dfs = [math.exp(-0.04 * t) for t in times]
    flat = ([10.0], [0.02])
    result = qm.xva_netting_set(
        times,
        dfs,
        (0.03, 0.01),
        [
            {
                "kind": "script",
                "script": "2028-10-01\n    pays 1000000\n",
                "valuation_date": "2026-10-02",
            }
        ],
        flat,
        flat,
        paths=2000,
    )
    assert result["pilot_paths"] >= 20_000
    assert result["trade_values_today"][0] == pytest.approx(
        1_000_000 * math.exp(-0.04 * 2.0), rel=2e-3
    )
    assert result["cva"]["value"] < 0 and result["dva"]["value"] == 0.0
    # No SA-CCR description of a script: the standardised approach says so
    # instead of guessing; the internal models method needs none.
    bond = [
        {
            "kind": "script",
            "script": "2028-10-01\n    pays 1000000\n",
            "valuation_date": "2026-10-02",
        }
    ]
    with pytest.raises(Exception, match="SA-CCR description of a scripted trade"):
        qm.xva_netting_set(
            times, dfs, (0.03, 0.01), bond, flat, flat, paths=1000, capital={"pd": 0.01}
        )
    modelled = qm.xva_netting_set(
        times,
        dfs,
        (0.03, 0.01),
        bond,
        flat,
        flat,
        paths=2000,
        capital={"pd": 0.01, "method": "internal_model"},
    )
    # A bond bought is an exposure of about its value all along: the EAD is
    # 1.4 times it, a little more as it accretes over the year.
    value = modelled["trade_values_today"][0]
    assert 1.4 * value < modelled["capital"]["ead_today"] < 1.4 * 1.06 * value
    assert modelled["kva"] < 0


# ── Lot X6: wrong-way risk ───────────────────────────────────────────────────


def test_wrong_way_risk_raises_the_cva_and_leaves_the_independent_one_alone():
    plain = run(portfolio="single_swap")
    adj = plain["adjustments"]
    # No dependence asked for: the two are the same numbers.
    assert adj["cva"] == adj["cva_independent"] and adj["dva"] == adj["dva_independent"]

    wrong = run(portfolio="single_swap", wrong_way_risk=10.0)["adjustments"]
    right = run(portfolio="single_swap", wrong_way_risk=-10.0)["adjustments"]
    for a in (wrong, right):
        assert a["cva_independent"] == adj["cva"]
    # Default where the swap is an asset: a dearer CVA. And a larger DVA:
    # where the bank owes, the counterparty now survives longer, so the
    # bank's own default is more often the first.
    assert wrong["cva"]["value"] < 1.3 * adj["cva"]["value"] < 0
    assert wrong["dva"]["value"] > adj["dva"]["value"]
    # The other way round for right-way risk.
    assert adj["cva"]["value"] < 0.8 * adj["cva"]["value"] < right["cva"]["value"] < 0
    assert right["dva"]["value"] < adj["dva"]["value"]
    assert "Wrong-way risk" in [s["title"] for s in plain["methodology"]]

    # Under a CSA the exposure is a ten-day move: the level of the value, and
    # so this hazard, hardly matters.
    csa = run(portfolio="single_swap", csa={}, wrong_way_risk=10.0)["adjustments"]
    ratio = csa["cva"]["value"] / csa["cva_independent"]["value"]
    assert 0.6 < ratio < 1.1
    assert (
        client.post(
            "/api/xva/netting-set", json={"paths": PATHS, "wrong_way_risk": 100}
        ).status_code
        == 422
    )


# ── Lot X5b: the margin from ISDA SIMM ───────────────────────────────────────


def test_the_margin_is_simm_when_every_trade_can_give_its_sensitivities():
    csa = {"cashflows": "withheld", "initial_margin": True}
    simm = run(portfolio="single_swap", csa=csa, borrowing_spread=0.005)
    margin = simm["initial_margin"]
    # Chosen for the user, and said.
    assert (margin["requested"], margin["model"]) == ("auto", "simm")
    assert "ISDA SIMM itself" in margin["reason"]
    today = margin["simm_today"]
    assert margin["today"] == pytest.approx(today["total"])
    # A swap: delta margin only. 60 bp on the PV01 of 10 M over 10 years.
    assert today["vega"] == 0.0 and today["curvature"] == 0.0
    assert 300_000 < today["delta"] < 700_000

    regression = run(
        portfolio="single_swap",
        csa={**csa, "initial_margin_model": "regression"},
        borrowing_spread=0.005,
    )
    # ISDA's risk weights are calibrated on stress: more margin than this
    # model's own 99 %, so a larger MVA and even less CVA.
    assert 1.2 < margin["today"] / regression["initial_margin"]["today"] < 2.2
    assert simm["adjustments"]["mva"] < regression["adjustments"]["mva"] < 0
    assert abs(simm["adjustments"]["cva"]["value"]) <= abs(
        regression["adjustments"]["cva"]["value"]
    )

    # An option adds vega and curvature.
    option = run(portfolio="bought_swaption", csa=csa)["initial_margin"]["simm_today"]
    assert option["delta"] > 0 and option["vega"] > 0 and option["curvature"] > 0


def test_a_trade_valued_by_regression_takes_the_regression_model_and_says_why():
    csa = {"cashflows": "withheld", "initial_margin": True}
    body = run(portfolio="bermudan", csa=csa)
    margin = body["initial_margin"]
    assert (margin["requested"], margin["model"]) == ("auto", "regression")
    assert "bermudan" in margin["reason"] and margin["simm_today"] is None
    assert margin["today"] > 0
    # Asked for by name, it is refused with the reason, not replaced.
    r = client.post(
        "/api/xva/netting-set",
        json={
            "paths": PATHS,
            "portfolio": "bermudan",
            "csa": {**csa, "initial_margin_model": "simm"},
        },
    )
    assert r.status_code == 422 and "valued by regression" in r.text
    # A margin given for today scales the regression; SIMM computes its own.
    r = client.post(
        "/api/xva/netting-set",
        json={
            "paths": PATHS,
            "csa": {
                **csa,
                "initial_margin_model": "simm",
                "initial_margin_today": 100_000,
            },
        },
    )
    assert r.status_code == 422 and "SIMM computes today" in r.text


# ── Lot X7: where the paths are valued ───────────────────────────────────────


def test_the_response_says_where_the_paths_ran_and_why(store):
    import quantmodeling as qm

    cards = len(qm.gpu_devices())
    cpu = run(device="cpu")
    assert (cpu["device"], cpu["gpus"]) == ("cpu", 0)
    assert cpu["device_reason"] == "The CPU was requested."
    assert store["events"][-1][1].engine.device == "cpu"

    # Left free, a swap goes to the cards when the server has some.
    auto = run()
    if cards:
        assert auto["device"] == "gpu" and 1 <= auto["gpus"] <= cards
        assert "closed form" in auto["device_reason"]
        # The same scenarios: the numbers of the CPU, to the last digits.
        assert auto["adjustments"]["cva"]["value"] == pytest.approx(
            cpu["adjustments"]["cva"]["value"], rel=1e-9
        )
        assert auto["exposure"]["ee"] == pytest.approx(cpu["exposure"]["ee"], rel=1e-9)
    else:
        assert (auto["device"], auto["gpus"]) == ("cpu", 0)
        assert auto["device_reason"].startswith("On the CPU: ")
        assert auto["adjustments"] == cpu["adjustments"]
    assert store["events"][-1][1].engine.device == auto["device"]

    # A Bermudan is valued by regression: on the CPU, whatever the server.
    bermudan = run(portfolio="bermudan")
    assert bermudan["device"] == "cpu"
    assert bermudan["device_reason"].startswith("On the CPU: ")
    if cards:
        assert "regression" in bermudan["device_reason"]

    # Asked for by name, the GPU is not replaced by the CPU.
    r = client.post(
        "/api/xva/netting-set",
        json={"paths": PATHS, "portfolio": "bermudan", "device": "gpu"},
    )
    assert r.status_code == 422 and "GPU requested" in r.text
    r = client.post("/api/xva/netting-set", json={"paths": PATHS, "device": "tpu"})
    assert r.status_code == 422


# ── Decision D4: scripted trades in the portfolios, and their capital ────────


def test_a_swap_written_as_a_script_is_the_swap():
    native = run(portfolio="single_swap", capital={"method": "internal_model"})
    scripted = run(portfolio="scripted_swap")
    (trade,) = scripted["trades"]
    assert trade["kind"] == "script" and "pays 10000000 * (" in trade["script"]
    assert "libor = 1 / df(" in trade["script"]
    # The terms it was written from are kept, and it is struck at par.
    assert trade["fixed_rate"] == pytest.approx(native["trades"][0]["fixed_rate"])
    assert (trade["tenor"], trade["maturity"]) == (10.0, 10.0)
    assert abs(trade["value_today"]) < 2e-3 * 10_000_000
    assert scripted["pilot_paths"] >= 20_000

    # Same exposure and CVA as the native swap, to the regression's accuracy.
    a, b = native["adjustments"], scripted["adjustments"]
    assert b["cva"]["value"] == pytest.approx(a["cva"]["value"], rel=0.06)
    assert b["dva"]["value"] == pytest.approx(a["dva"]["value"], rel=0.06)
    assert max(scripted["exposure"]["ee"]) == pytest.approx(
        max(native["exposure"]["ee"]), rel=0.06
    )

    # No supervisory description: the capital is read off the simulation, and
    # the page is told why.
    k = scripted["capital"]
    assert k["method"] == "internal_model" and "script" in k["method_reason"]
    assert k["ead_today"] == pytest.approx(native["capital"]["ead_today"], rel=0.06)
    # 1.4 × the Effective EPE of the pricing simulation's own profile.
    assert k["ead_today"] == pytest.approx(1.4 * scripted["exposure"]["eepe"], rel=1e-6)
    assert b["kva"] < 0
    assert b["kva"] == pytest.approx(a["kva"], rel=0.08)
    # A script is valued by regression: on the CPU, and no SIMM.
    assert scripted["device"] == "cpu"
    margined = run(portfolio="scripted_swap", csa={"initial_margin": True})
    assert margined["initial_margin"]["model"] == "regression"
    assert "script" in margined["initial_margin"]["reason"]
    assert margined["capital"]["ead_today"] < 0.3 * k["ead_today"]


def test_the_capital_method_is_chosen_announced_and_can_be_set():
    standard = run(portfolio="single_swap")["capital"]
    assert standard["method"] == "sa_ccr" and "SA-CCR" in standard["method_reason"]
    modelled = run(portfolio="single_swap", capital={"method": "internal_model"})
    k = modelled["capital"]
    assert (k["method"], k["method_reason"]) == ("internal_model", "Chosen by hand.")
    # The internal model: 1.4 × the Effective EPE of the pricing simulation,
    # below the standardised figure, which is built to be conservative.
    assert 0.2 * standard["ead_today"] < k["ead_today"] < standard["ead_today"]
    assert modelled["adjustments"]["kva"] < 0
    # Asked for by name on a script, the standardised approach is refused.
    r = client.post(
        "/api/xva/netting-set",
        json={
            "paths": PATHS,
            "portfolio": "scripted_swap",
            "capital": {"method": "sa_ccr"},
        },
    )
    assert r.status_code == 422 and "supervisory description" in r.text


def test_the_scripts_of_the_request_are_trades_of_the_netting_set():
    end = (AS_OF.replace(year=AS_OF.year + 3)).isoformat()
    bond = {"script": f"{end}\n    pays 1000000\n", "label": "Zero-coupon bond 3Y"}
    alone = run(portfolio="custom", scripts=[bond])
    (trade,) = alone["trades"]
    assert (trade["kind"], trade["description"]) == ("script", "Zero-coupon bond 3Y")
    assert trade["notional"] is None and trade["script"] == bond["script"]
    assert trade["maturity"] == pytest.approx(3.0, abs=0.01)
    # A bond bought: an asset until it pays, so a CVA and no DVA.
    assert 800_000 < trade["value_today"] < 1_000_000
    assert alone["adjustments"]["cva"]["value"] < 0
    assert alone["adjustments"]["dva"]["value"] == 0.0
    assert alone["capital"]["method"] == "internal_model"

    # Added to a portfolio, it nets with it; sold, it is the other side.
    both = run(portfolio="single_swap", scripts=[{**bond, "quantity": -1.0}])
    assert [t["kind"] for t in both["trades"]] == ["swap", "script"]
    assert both["trades"][1]["value_today"] == pytest.approx(
        -trade["value_today"], rel=5e-3
    )
    assert both["capital"]["method"] == "internal_model"

    # What cannot be priced is the request's error, with the reason.
    def refused(**body):
        r = client.post("/api/xva/netting-set", json={"paths": PATHS, **body})
        assert r.status_code == 422, r.text
        return r.text

    assert "at least one" in refused(portfolio="custom")
    assert "Script 1" in refused(portfolio="custom", scripts=[{"script": "pays 1 +"}])
    past = {"script": "2020-01-01\n    pays 1\n"}
    assert "historical fixing for 2020-01-01" in refused(
        portfolio="custom", scripts=[past]
    )
    equity = {"script": f"{end}\n    pays max(spot() - 100, 0)\n"}
    assert "spot" in refused(portfolio="custom", scripts=[equity])
    assert refused(portfolio="custom", scripts=[bond] * 4)


# ── Lot X8: sensitivities by adjoint differentiation, and SA-CVA ─────────────


def sensitivities(**body) -> dict:
    r = client.post("/api/xva/sensitivities", json={"paths": PATHS, **body})
    assert r.status_code == 200, r.text
    return r.json()


def test_the_sensitivities_of_a_swaps_cva_to_every_quote():
    body = sensitivities(portfolio="single_swap", borrowing_spread=0.004)
    adj = body["adjustments"]
    assert adj["cva"]["value"] < 0 < adj["dva"]["value"] and adj["fca"]["value"] < 0
    # The bank's own default removes scenarios: the bilateral CVA is smaller.
    assert adj["cva_unilateral"]["value"] < adj["cva"]["value"]
    # The same netting set as the valuation endpoint, to the credit curve
    # (one spread at five tenors here, one hazard rate there).
    valued = run(portfolio="single_swap")["adjustments"]["cva"]["value"]
    assert adj["cva"]["value"] == pytest.approx(valued, rel=0.02)

    # One sensitivity per quote, each with its error.
    assert [q["tenor"] for q in body["swap_rates"]] == [float(t) for t in PAR]
    assert [(q["expiry"], q["tenor"]) for q in body["swaption_vols"]] == [
        (float(e), float(t)) for e, t, _ in VOLS
    ]
    assert [q["tenor"] for q in body["counterparty_spreads"]] == [0.5, 1, 3, 5, 10]
    assert body["swap_rates"][5]["label"] == "swap rate 10Y"
    ten = body["swap_rates"][5]["cva"]
    # A payer swap at par: a higher 10-year rate puts it in the money, so the
    # CVA, a cost, grows. It is the largest of the swap-rate sensitivities.
    assert ten["value"] < 0 and 0 < ten["error"] < 0.1 * abs(ten["value"])
    assert abs(ten["value"]) == max(abs(q["cva"]["value"]) for q in body["swap_rates"])
    # Nothing is paid after ten years: the longer rates do not matter.
    assert body["swap_rates"][-1]["cva"]["value"] == 0.0
    # More volatility, more exposure; a wider counterparty spread, more CVA.
    assert sum(q["cva"]["value"] for q in body["swaption_vols"]) < 0
    assert sum(q["cva"]["value"] for q in body["counterparty_spreads"]) < 0
    assert sum(q["dva"]["value"] for q in body["counterparty_spreads"]) < 0
    # CVA is linear in the loss given default, FCA in the borrowing spread.
    others = {q["label"]: q for q in body["others"]}
    lgd = others["counterparty loss given default"]
    assert lgd["cva"]["value"] == pytest.approx(adj["cva"]["value"] / 0.6, rel=1e-9)
    assert others["borrowing spread"]["fca"]["value"] == pytest.approx(
        adj["fca"]["value"] / 0.004, rel=1e-9
    )
    # The model's own inputs are there too.
    labels = [m["label"] for m in body["model_risks"]]
    assert "Hull-White volatility" in labels and "zero rate 10Y" in labels
    assert body["inputs"] == len(labels)

    # What it cost: one run for everything, against two per input by bumping.
    assert body["bump_valuations"] == 2 * body["inputs"]
    assert 1 < body["cost_ratio"] < body["bump_valuations"]
    assert body["hull_white"]["sigma"] > 0 and body["market_as_of"] == AS_OF.isoformat()
    titles = [s["title"] for s in body["methodology"]]
    assert "Adjoint differentiation" in titles and "SA-CVA" in titles


def test_sa_cva_from_the_sensitivities():
    body = sensitivities(portfolio="directional", sector="financial")
    sa = body["sa_cva"]
    assert sa["interest_rate_tenors"] == [1, 2, 5, 10, 30]
    assert sa["interest_rate_risk_weights"] == [0.0111, 0.0093, 0.0074, 0.0074, 0.0074]
    assert sa["credit_spread_tenors"] == [0.5, 1, 3, 5, 10]
    # BBB, a financial: investment grade, bucket 2 of MAR50.65.
    assert (sa["sector"], sa["investment_grade"]) == ("financial", True)
    assert sa["credit_spread_risk_weight"] == 0.05
    # The swap-rate sensitivities of the unilateral CVA, spread over the five
    # tenors, add up to their sum; the vega is every vol moved by the same share.
    unilateral = [q["cva_unilateral"]["value"] for q in body["swap_rates"]]
    assert sum(sa["interest_rate_delta"]) == pytest.approx(sum(unilateral), rel=1e-9)
    assert sa["interest_rate_vega"] == pytest.approx(
        sum(q["level"] * q["cva_unilateral"]["value"] for q in body["swaption_vols"]),
        rel=1e-9,
    )
    assert sa["capital_interest_rate_vega"] == pytest.approx(
        abs(sa["interest_rate_vega"])
    )
    # The classes add up, and the counterparty's spread dominates.
    parts = (
        sa["capital_interest_rate_delta"]
        + sa["capital_interest_rate_vega"]
        + sa["capital_credit_spread_delta"]
    )
    assert sa["capital"] == pytest.approx(parts) and sa["capital"] > 0
    assert sa["capital_credit_spread_delta"] > sa["capital_interest_rate_delta"]
    # A high-yield counterparty: 12 % instead of 5 %.
    risky = sensitivities(
        portfolio="directional", sector="financial", counterparty_rating="B"
    )["sa_cva"]
    assert not risky["investment_grade"] and risky["credit_spread_risk_weight"] == 0.12


def test_sensitivities_under_a_csa_and_what_is_not_differentiated():
    open_set = sensitivities(portfolio="balanced")
    margined = sensitivities(portfolio="balanced", csa={})
    # Variation margin leaves a fraction of the CVA and of its sensitivities.
    assert abs(margined["adjustments"]["cva"]["value"]) < 0.5 * abs(
        open_set["adjustments"]["cva"]["value"]
    )
    spread = lambda b: sum(q["cva"]["value"] for q in b["counterparty_spreads"])
    assert abs(spread(margined)) < 0.5 * abs(spread(open_set))
    assert not any("minimum transfer" in w for w in margined["warnings"])
    with_mta = sensitivities(portfolio="balanced", csa={"minimum_transfer_amount": 5e4})
    assert any("minimum transfer" in w for w in with_mta["warnings"])

    def refused(**body):
        r = client.post("/api/xva/sensitivities", json={"paths": PATHS, **body})
        assert r.status_code == 422, r.text
        return r.text

    assert "valued by regression" in refused(portfolio="bermudan")
    assert "valued by regression" in refused(portfolio="scripted_swap")
    assert "scripts" in refused(portfolio="custom")
    assert "initial margin" in refused(csa={"initial_margin": True})
    assert refused(paths=50_000)
