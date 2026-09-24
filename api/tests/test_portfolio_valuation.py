"""Portfolio valuation on a controlled fake market (db replaced).

Properties: daily P&L summed over a portfolio's life equals its total P&L
(realised + unrealised), in one currency and across currencies; a purchase
day's P&L is quantity × (close − price) − fees; a derivative is revalued with
the day's spot and the time left to expiry, and says which inputs are
proxied; an expired derivative is not valued; a past day's mark is computed
once; old portfolios are migrated on the fly.
"""

from __future__ import annotations

import math
import os
from datetime import date, timedelta

os.environ.setdefault("JWT_SECRET", "test-only-secret-not-for-deployment")

import pandas as pd
import pytest
from fastapi.testclient import TestClient

from api.app import db, fx, valuation
from api.app import portfolio_valuation as pv
from api.app.main import app
from api.app.portfolio_schemas import (
    DerivativeSpec,
    EquitySpec,
    Instrument,
    Portfolio,
    Trade,
)

START = date(2026, 6, 1)
END = date(2026, 9, 18)
DAYS = [d.date() for d in pd.bdate_range(START - timedelta(days=200), END)]


def _path(start: float, drift: float, wiggle: float):
    return {
        d: start * math.exp(drift * i + wiggle * math.sin(i / 3.0))
        for i, d in enumerate(DAYS)
    }


CLOSES = {
    "AAA": _path(100.0, 0.0004, 0.02),  # EUR
    "BBB": _path(50.0, -0.0002, 0.03),  # USD
}
CCY = {"AAA": "EUR", "BBB": "USD"}
EURUSD = _path(1.10, 0.0001, 0.01)  # USD per EUR


class DictStorage:
    def __init__(self):
        self.data = {}

    def read_json(self, key):
        return self.data.get(key)

    def write_json(self, key, value):
        import json

        self.data[key] = json.loads(json.dumps(value))


@pytest.fixture(autouse=True)
def market(monkeypatch):
    monkeypatch.setattr(
        db,
        "price_history",
        lambda t, since: [(d, v) for d, v in sorted(CLOSES[t].items()) if d >= since],
    )
    monkeypatch.setattr(db, "ticker_currency", lambda t: CCY.get(t))

    def ecb(currencies, since):
        frame = pd.DataFrame({"USD": pd.Series(EURUSD)}).sort_index()
        frame["EUR"] = 1.0
        frame = frame[[c for c in currencies]]
        return frame[frame.index >= since] if since else frame

    monkeypatch.setattr(db, "ecb_fx_history", ecb)
    monkeypatch.setattr(db, "rates_curve_history", lambda table, ids, since: [])
    monkeypatch.setattr(db, "dividend_yield_history", lambda t, since: [])
    store = DictStorage()
    monkeypatch.setattr(pv, "get_storage", lambda: store)
    monkeypatch.setattr(pv, "_STORE", pv.MarkStore())
    return store


def _equity_pf(base="EUR", trades=None) -> Portfolio:
    return Portfolio(
        id="p",
        version=2,
        base_currency=base,
        instruments=[
            Instrument(id="EQ:AAA", label="AAA", spec=EquitySpec(ticker="AAA")),
            Instrument(id="EQ:BBB", label="BBB", spec=EquitySpec(ticker="BBB")),
        ],
        transactions=trades
        or [
            Trade(
                id="1",
                instrument_id="EQ:AAA",
                trade_date=date(2026, 6, 2),
                quantity=100,
                price=101.0,
                fees=3,
            ),
            Trade(
                id="2",
                instrument_id="EQ:AAA",
                trade_date=date(2026, 7, 6),
                quantity=-40,
                price=104.0,
                fees=2,
            ),
            Trade(
                id="3",
                instrument_id="EQ:AAA",
                trade_date=date(2026, 8, 3),
                quantity=-90,
                price=106.0,
                fees=2,
            ),
        ],
    )


def test_daily_pnl_sums_to_the_total_pnl_in_one_currency():
    pf = _equity_pf()
    points, _ = pv.history(pf, START, END)
    snap = pv.snapshot(pf, END)
    assert points[-1].cumulative_pnl == pytest.approx(snap.total_pnl, abs=1e-8)
    assert snap.total_pnl == pytest.approx(snap.realised + snap.unrealised, abs=1e-8)
    # the short opened by selling 90 against 60 held is still open
    aaa = next(p for p in snap.positions if p.instrument_id == "EQ:AAA")
    assert aaa.quantity == pytest.approx(-30)


def test_a_window_after_the_first_trade_still_ends_on_the_total_pnl():
    pf = _equity_pf()
    points, _ = pv.history(pf, date(2026, 8, 20), END)
    assert points[0].date == date(2026, 8, 20)
    assert points[-1].cumulative_pnl == pytest.approx(
        pv.snapshot(pf, END).total_pnl, abs=1e-8
    )


def test_and_across_currencies_fx_included():
    trades = [
        Trade(
            id="1",
            instrument_id="EQ:BBB",
            trade_date=date(2026, 6, 2),
            quantity=200,
            price=49.0,
            fees=1,
        ),
        Trade(
            id="2",
            instrument_id="EQ:AAA",
            trade_date=date(2026, 6, 10),
            quantity=10,
            price=100.0,
        ),
        Trade(
            id="3",
            instrument_id="EQ:BBB",
            trade_date=date(2026, 8, 12),
            quantity=-50,
            price=51.0,
            fees=1,
        ),
    ]
    pf = _equity_pf(base="EUR", trades=trades)
    points, _ = pv.history(pf, START, END)
    snap = pv.snapshot(pf, END)
    assert points[-1].cumulative_pnl == pytest.approx(snap.total_pnl, abs=1e-8)


def test_a_purchase_days_pnl_is_quantity_times_close_minus_price_minus_fees():
    pf = _equity_pf(
        trades=[
            Trade(
                id="1",
                instrument_id="EQ:AAA",
                trade_date=date(2026, 6, 2),
                quantity=100,
                price=101.0,
                fees=3,
            )
        ]
    )
    points, _ = pv.history(pf, START, END)
    first = next(p for p in points if p.date == date(2026, 6, 2))
    assert first.daily_pnl == pytest.approx(
        100 * (CLOSES["AAA"][date(2026, 6, 2)] - 101.0) - 3
    )


def _option_pf(expiry=date(2027, 3, 19)) -> Portfolio:
    return Portfolio(
        id="o",
        version=2,
        base_currency="EUR",
        instruments=[
            Instrument(
                id="opt",
                label="AAA call",
                spec=DerivativeSpec(
                    product="vanilla",
                    underlying="AAA",
                    currency="EUR",
                    expiry=expiry,
                    params={
                        "spot": 1.0,
                        "strike": 105.0,
                        "maturity": 9.9,
                        "rate": 0.02,
                        "dividend": 0.0,
                        "vol": 0.9,
                        "is_call": True,
                    },
                ),
            )
        ],
        transactions=[
            Trade(
                id="1",
                instrument_id="opt",
                trade_date=date(2026, 6, 2),
                quantity=10,
                price=6.0,
            )
        ],
    )


def test_a_derivative_is_revalued_with_the_days_market():
    snap = pv.snapshot(_option_pf(), END)
    [pos] = snap.positions
    inputs = {i.name.split(" ")[0]: i for i in pos.inputs}
    assert inputs["time"].value == pytest.approx(
        (date(2027, 3, 19) - END).days / 365.25
    )
    assert inputs["spot"].value == pytest.approx(CLOSES["AAA"][END])
    assert inputs["vol"].status == "proxied"  # realised vol, not the 90 % typed in
    assert inputs["rate"].status == "default"  # no curve in this fake market
    # the mark is the vanilla price on exactly those inputs
    req = valuation.PRODUCTS["vanilla"].request.model_validate(
        {
            "spot": CLOSES["AAA"][END],
            "strike": 105.0,
            "maturity": inputs["time"].value,
            "rate": 0.02,
            "dividend": 0.0,
            "vol": inputs["vol"].value,
            "is_call": True,
        }
    )
    assert pos.mark == pytest.approx(valuation.price("vanilla", req).response.npv)


def test_an_expired_derivative_is_not_valued_and_says_what_to_do():
    snap = pv.snapshot(_option_pf(expiry=date(2026, 9, 1)), END)
    [pos] = snap.positions
    assert pos.mark is None and "settlement" in pos.note
    assert any("settlement" in w for w in snap.warnings)


def test_a_past_days_mark_is_computed_once(monkeypatch, market):
    calls = []
    real = valuation.price
    monkeypatch.setattr(valuation, "price", lambda *a: calls.append(1) or real(*a))
    pf = _option_pf()
    pv.history(pf, date(2026, 9, 1), END)
    first = len(calls)
    monkeypatch.setattr(pv, "_STORE", pv.MarkStore())  # a fresh process: memory empty
    pv.history(pf, date(2026, 9, 1), END)
    assert first > 0 and len(calls) == first  # every past mark read back, none repriced
    assert any(k.startswith(pv.MarkStore.PREFIX) for k in market.data)


# ── Endpoints ────────────────────────────────────────────────────────────────


def test_snapshot_endpoint_migrates_an_old_portfolio_on_the_fly():
    legacy = {
        "id": "old",
        "created_at": "2026-06-02T10:00:00",
        "positions": [
            {
                "id": "a",
                "label": "Call",
                "product_type": "vanilla",
                "quantity": 1,
                "entry_price": 5,
                "parameters": {
                    "spot": 100,
                    "strike": 100,
                    "maturity": 1,
                    "rate": 2,
                    "dividend": 0,
                    "vol": 20,
                    "is_call": True,
                },
            }
        ],
    }
    r = TestClient(app).post(
        "/api/portfolio-valuation/snapshot",
        json={"portfolio": legacy, "as_of": "2026-09-18"},
    )
    assert r.status_code == 200, r.text
    assert r.json()["positions"][0]["quantity"] == 1


def test_a_trade_on_an_unknown_instrument_is_refused():
    pf = _equity_pf().model_dump(mode="json")
    pf["transactions"][0]["instrument_id"] = "EQ:ZZZ"
    r = TestClient(app).post(
        "/api/portfolio-valuation/snapshot", json={"portfolio": pf}
    )
    assert r.status_code == 422 and "unknown instruments" in r.json()["message"]


def test_close_endpoint_gives_the_close_on_or_before_a_date():
    r = (
        TestClient(app)
        .get("/api/portfolio-valuation/close?ticker=AAA&date=2026-09-19")
        .json()
    )
    assert r["date"] == "2026-09-18" and r["currency"] == "EUR"
    assert r["close"] == pytest.approx(CLOSES["AAA"][date(2026, 9, 18)])
