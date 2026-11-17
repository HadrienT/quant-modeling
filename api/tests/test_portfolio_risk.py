"""Stress tests and value at risk of the trade ledger (issue #292), on the
fake market of test_portfolio_valuation (db replaced).

Properties: a share moves exactly with the spot shock and with nothing else; a
bought call gains with the spot and with the volatility, monotonically; a
stressed mark is never stored with the official ones; a VaR scenario is the
holdings applied to the day's change of each instrument; the VaR's interval
holds it and the expected shortfall is beyond it.
"""

from __future__ import annotations

import math
import os
import time
from datetime import date

os.environ.setdefault("JWT_SECRET", "test-only-secret-not-for-deployment")

import pytest
from fastapi.testclient import TestClient

from api.app import portfolio_risk as pr
from api.app import portfolio_valuation as pv
from api.app.main import app
from api.app.portfolio_schemas import EquitySpec, Instrument, Portfolio, Trade

from test_portfolio_valuation import CLOSES, END, _option_pf, market  # noqa: F401


def _bbb(qty: float = 100.0, bought: date = date(2026, 6, 1)) -> Portfolio:
    return Portfolio(
        id="b",
        version=2,
        base_currency="USD",
        instruments=[
            Instrument(id="EQ:BBB", label="BBB", spec=EquitySpec(ticker="BBB"))
        ],
        transactions=[
            Trade(
                id="t1",
                instrument_id="EQ:BBB",
                trade_date=bought,
                quantity=qty,
                price=50.0,
            )
        ],
    )


def _by_name(results):
    return {r.scenario.name: r for r in results}


def test_a_share_moves_with_the_spot_shock_and_nothing_else():
    res, warnings = pr.stress(_bbb(), END)
    assert warnings == []
    s = _by_name(res)
    value = 100.0 * CLOSES["BBB"][END]
    assert s["Equities −10%"].pnl == pytest.approx(-0.10 * value, rel=1e-12)
    assert s["Equities +20%"].pnl == pytest.approx(0.20 * value, rel=1e-12)
    for name in ("Volatility +5 pts", "Rates +100 bp"):
        assert s[name].pnl == 0.0 and s[name].not_revalued == []
    assert s["Equities −10%"].by_underlying == {"BBB": pytest.approx(-0.10 * value)}


def test_a_bought_call_gains_with_the_spot_and_the_volatility():
    res, _ = pr.stress(_option_pf(), END)
    s = _by_name(res)
    ladder = [
        s[n].pnl
        for n in ("Equities −20%", "Equities −10%", "Equities +10%", "Equities +20%")
    ]
    assert ladder == sorted(ladder) and ladder[1] < 0 < ladder[2]
    assert s["Volatility +5 pts"].pnl > 0 > s["Volatility −5 pts"].pnl
    assert s["Crash"].pnl < 0
    assert all(r.not_revalued == [] for r in res)


def test_a_stressed_mark_is_never_stored(market):  # noqa: F811
    pf = _option_pf()
    pv.snapshot(pf, END)
    stored = dict(market.data)
    pr.stress(pf, END)
    assert market.data == stored


def test_a_var_scenario_is_the_holdings_on_the_days_change():
    v = pr.value_at_risk(_bbb(), END, window=100)
    assert v.days_dropped == 0 and v.instruments == 1 and len(v.scenarios) == 100
    today = CLOSES["BBB"][END]
    d1 = v.dates[0]
    prev = pv.previous_business_day(d1)
    expected = 100.0 * today * (CLOSES["BBB"][d1] / CLOSES["BBB"][prev] - 1.0)
    assert v.scenarios[0] == pytest.approx(expected, rel=1e-12)
    for level in v.levels:
        k = math.ceil((1 - level.confidence) * len(v.scenarios))
        assert level.var.value == pytest.approx(-sorted(v.scenarios)[k - 1])
        assert level.var.lower <= level.var.value <= level.var.upper
        assert level.expected_shortfall.value >= level.var.value


def test_a_window_without_enough_marks_is_refused():
    # The fake market starts in November 2025: some forty days before this.
    with pytest.raises(ValueError, match="at least 60"):
        pr.value_at_risk(_bbb(bought=date(2025, 12, 1)), date(2026, 1, 15), window=250)


def test_the_api_runs_the_var_as_a_task_and_the_stress_at_once():
    client = TestClient(app)
    body = {"portfolio": _bbb().model_dump(mode="json"), "as_of": END.isoformat()}
    stress = client.post("/api/portfolio-valuation/stress", json=body)
    assert stress.status_code == 200, stress.text
    assert len(stress.json()["scenarios"]) == len(pr.SCENARIOS)
    started = client.post("/api/portfolio-valuation/var", json={**body, "window": 100})
    assert started.status_code == 202, started.text
    task_id = started.json()["id"]
    deadline = time.time() + 30
    while time.time() < deadline:
        t = client.get(f"/api/portfolio-valuation/var/{task_id}").json()
        if t["status"] not in ("queued", "running"):
            break
        time.sleep(0.05)
    assert t["status"] == "done", t
    assert [L["confidence"] for L in t["var"]["levels"]] == [0.99, 0.975]
    assert client.get("/api/portfolio-valuation/var/nope").status_code == 404


def test_the_var_announced_each_day_is_the_one_of_the_day_before():
    pf = _bbb(bought=date(2026, 3, 2))
    h = pr.var_history(pf, END, days=60, window=60)
    assert len(h.dates) == 60 and h.skipped == 0
    for t, var, pnl in list(zip(h.dates, h.var, h.pnl))[::15]:
        prev = pv.previous_business_day(t)
        assert var == pytest.approx(
            pr.value_at_risk(pf, prev, window=60).levels[0].var.value
        )
        assert pnl == pytest.approx(100.0 * (CLOSES["BBB"][t] - CLOSES["BBB"][prev]))


def test_the_risk_model_tab_runs_the_wheels_tests():
    from api.app.backtest import risk_model

    r = risk_model.run_risk_model(
        risk_model.RiskModelRequest(
            portfolio=_bbb(bought=date(2026, 3, 2)), end=END, days=60, window=60
        )
    )
    assert r.observations == 60 == len(r.pnl) == len(r.value_at_risk)
    assert r.exceptions == sum(r.exception_days)
    assert r.expected_exceptions == pytest.approx(0.6)
    assert r.traffic_light.zone and set(r.data.fingerprints) == {"portfolio", "marks"}
