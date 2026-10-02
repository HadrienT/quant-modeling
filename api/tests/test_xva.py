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
        csa={**csa, "initial_margin": True},
        borrowing_spread=0.005,
    )
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
    # No PD given: the market-implied one, and the page is told.
    assert k["pd_is_market_implied"]
    assert any("higher than the default rate" in w for w in body["warnings"])
    assert (k["lgd"], k["sector"], k["investment_grade"]) == (0.40, "other", True)

    # A bank's own PD, lower: less default capital, no warning.
    own = run(portfolio="single_swap", capital={"pd": 0.002})
    assert not own["capital"]["pd_is_market_implied"]
    assert own["capital"]["default_capital_today"] < k["default_capital_today"]
    assert own["capital"]["cva_capital_today"] == pytest.approx(k["cva_capital_today"])
    assert not any("higher than the default rate" in w for w in own["warnings"])
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
    # No SA-CCR description of a script: the capital says so instead of guessing.
    with pytest.raises(Exception, match="SA-CCR description of a scripted trade"):
        qm.xva_netting_set(
            times,
            dfs,
            (0.03, 0.01),
            [
                {
                    "kind": "script",
                    "script": "2028-10-01\n    pays 1\n",
                    "valuation_date": "2026-10-02",
                }
            ],
            flat,
            flat,
            paths=1000,
            capital={"pd": 0.01},
        )


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
