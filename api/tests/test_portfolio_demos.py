"""Demo portfolios on the fake market of test_portfolio_valuation: every
trade is priced from the market on its own date (a stock at its close, an
option at its mark), strikes land on the listed grid, and a demo whose date
has no close is left out rather than priced at an older one."""

from __future__ import annotations

from datetime import date

import pytest
from fastapi.testclient import TestClient

from api.app import portfolio_demos as demos
from api.app import portfolio_valuation as pv
from api.app.main import app
from api.tests.test_portfolio_valuation import CLOSES, END, market  # noqa: F401

DAY = date(2026, 6, 2)


def _demo(on: date = DAY) -> demos.Demo:
    return demos.Demo(
        id="demo-test",
        name="Test",
        description="",
        base_currency="EUR",
        options=(demos.Option("c", "AAA", True, 1.10, date(2027, 3, 19), "EUR"),),
        trades=(
            demos.Buy(on, "AAA", 10, 2.0),
            demos.Buy(on, "c", -10, 0.0),
            demos.Buy(date(2026, 7, 6), "AAA", -4, 2.0),
        ),
    )


def test_every_trade_is_priced_from_the_market_on_its_own_date():
    md = pv.MarketData(DAY)
    pf = demos.build(_demo(), md)
    by_id = {i.id: i for i in pf.instruments}
    stock = [t for t in pf.transactions if t.instrument_id == "EQ:AAA"]
    for t in stock:
        assert t.price == pytest.approx(CLOSES["AAA"][t.trade_date], abs=1e-4)
    [opt] = [t for t in pf.transactions if t.instrument_id == "c"]
    assert opt.price == pytest.approx(pv.mark(by_id["c"], DAY, md).value, abs=1e-4)


def test_an_option_strike_is_the_moneyness_rounded_to_the_listed_grid():
    pf = demos.build(_demo(), pv.MarketData(DAY))
    [call] = [i for i in pf.instruments if i.id == "c"]
    spot = CLOSES["AAA"][DAY]
    step = demos._strike_step(spot)
    strike = call.spec.params["strike"]
    assert strike / step == pytest.approx(round(strike / step))
    assert abs(strike - 1.10 * spot) <= step / 2 + 1e-9


def test_a_date_without_a_close_makes_the_demo_unavailable():
    saturday = date(2026, 6, 6)
    with pytest.raises(LookupError, match="no close"):
        demos.build(_demo(on=saturday), pv.MarketData(saturday))


def test_the_shipped_demos_are_well_formed():
    for d in demos.DEMOS:
        keys = {o.key for o in d.options}
        assert d.id.startswith("demo-")
        assert all(t.on.weekday() < 5 for t in d.trades)
        assert all(o.expiry > t.on for o in d.options for t in d.trades)
        assert keys <= {t.what for t in d.trades}


def test_the_endpoint_serves_the_demos_that_could_be_built(monkeypatch):
    monkeypatch.setattr(demos, "DEMOS", (_demo(), _demo(on=date(2026, 6, 6))))
    demos._built.cache_clear()
    try:
        r = TestClient(app).get("/api/portfolio-valuation/demos")
    finally:
        demos._built.cache_clear()
    assert r.status_code == 200
    [only] = r.json()
    assert only["portfolio"]["id"] == "demo-test"
    assert only["portfolio"]["version"] == 2
