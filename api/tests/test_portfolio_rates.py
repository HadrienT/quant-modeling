"""Rate and FX trades in the portfolio ledger, on a controlled fake market
(db replaced): flat-ish swap curves that move every day.

Properties: a trade struck at the market is worth zero that day; a payer and
a receiver are opposite; the P&L of a swap summed over its life is its mark
(nothing is lost when a coupon is paid); a date without a curve has no mark;
payer − receiver swaption = forward swap (parity).
"""

from __future__ import annotations

import math
import os
from datetime import date, timedelta

os.environ.setdefault("JWT_SECRET", "test-only-secret-not-for-deployment")

import pandas as pd
import pytest
from fastapi.testclient import TestClient

from api.app import db
from api.app import portfolio_rates as pr
from api.app import portfolio_valuation as pv
from api.app.main import app
from api.app.portfolio_schemas import Instrument, Portfolio, RateFxSpec, Trade
from api.tests.test_portfolio_valuation import DictStorage

START = date(2024, 10, 1)
END = date(2026, 9, 18)
DAYS = [d.date() for d in pd.bdate_range(START - timedelta(days=30), END)]
TENORS = [1, 2, 3, 5, 7, 10, 15, 20, 30]
#: A day DTCC published nothing, and the week around it.
GAP = [d for d in DAYS if date(2025, 3, 3) <= d <= date(2025, 3, 14)]


def _level(ccy: str, d: date) -> float:
    i = (d - DAYS[0]).days
    base = 0.04 if ccy == "USD" else 0.025
    return base + 0.004 * math.sin(i / 40.0) + 0.000002 * i


def _curves(product: str, since: date) -> dict:
    ccy = "USD" if product == pr.SWAP_PRODUCT["USD"] else "EUR"
    return {
        d: [db.SwapRate(t, _level(ccy, d) + 0.0005 * math.log(t), 10) for t in TENORS]
        for d in DAYS
        if d >= since and d not in GAP
    }


EURUSD = {d: 1.10 + 0.03 * math.sin((d - DAYS[0]).days / 55.0) for d in DAYS}


@pytest.fixture(autouse=True)
def market(monkeypatch):
    monkeypatch.setattr(db, "dtcc_swap_curves", _curves)
    monkeypatch.setattr(db, "dtcc_swaption_trades", lambda underlier, since: [])

    def ecb(currencies, since):
        frame = pd.DataFrame({"USD": pd.Series(EURUSD)}).sort_index()
        frame["EUR"] = 1.0
        frame = frame[list(currencies)]
        return frame[frame.index >= since] if since else frame

    monkeypatch.setattr(db, "ecb_fx_history", ecb)
    monkeypatch.setattr(db, "price_history", lambda t, since: [])
    monkeypatch.setattr(db, "ticker_currency", lambda t: None)
    monkeypatch.setattr(db, "rates_curve_history", lambda table, ids, since: [])
    store = DictStorage()
    monkeypatch.setattr(pv, "get_storage", lambda: store)
    monkeypatch.setattr(pv, "_STORE", pv.MarkStore())
    return store


def _draft(product="swap", currency="USD", years=5, **more) -> dict:
    return {
        "product": product,
        "currency": currency,
        "notional": 10_000_000.0,
        "payer": True,
        "start": START.isoformat(),
        "maturity": START.replace(year=START.year + years).isoformat(),
        **more,
    }


def _md() -> pv.MarketData:
    return pv.MarketData(START)


@pytest.mark.parametrize(
    "draft",
    [
        _draft(),
        _draft(currency="EUR", years=10),
        _draft("fx_forward", "EUR", 2),
        _draft("cross_currency_swap", "EUR", 5),
    ],
)
def test_a_trade_struck_at_the_market_is_worth_zero_that_day(draft):
    md = _md()
    spec = pr.at_market(draft, START, md)
    m = pr.mark(spec, START, md)
    assert m.value == pytest.approx(0.0, abs=1e-6 * spec.notional)
    assert m.currency == spec.value_currency


def test_the_other_side_is_worth_the_opposite():
    md = _md()
    d = date(2025, 11, 14)  # after a coupon, mid-period
    for draft in (_draft(), _draft("cross_currency_swap", "EUR", 5)):
        payer = pr.at_market(draft, START, md)
        receiver = payer.model_copy(update={"payer": False})
        a, b = pr.mark(payer, d, md), pr.mark(receiver, d, md)
        assert a.value != 0 and a.value == pytest.approx(-b.value)


def test_a_coupon_paid_does_not_move_the_mark():
    md = _md()
    spec = pr.at_market(_draft(), START, md)
    coupon = date(2025, 10, 1)
    before = pr.swap_value(spec, coupon - timedelta(days=1), md.rates_history)
    after = pr.swap_value(spec, coupon, md.rates_history)
    # The value left drops by the coupon, which moves to what was paid.
    assert before[1] == 0.0 and after[1] != 0.0
    assert after[0] + after[1] == pytest.approx(
        before[0] + before[1], abs=2e-3 * spec.notional
    )
    fixing = md.rates_history.fixing("USD", START)
    assert after[1] == pytest.approx(spec.notional * (fixing - spec.fixed_rate))


def test_a_date_without_a_curve_has_no_mark_and_says_why():
    md = _md()
    spec = pr.at_market(_draft(), START, md)
    m = pr.mark(spec, GAP[-1], md)
    assert m.value is None and "no USD swap curve" in m.note
    # Within a week of the last curve it is that curve, flagged stale.
    near = pr.mark(spec, GAP[3], md)
    assert near.value is not None and near.inputs[0].status == "stale"


def test_a_matured_swap_is_not_valued():
    md = _md()
    spec = pr.at_market(_draft(years=1), START, md)
    m = pr.mark(spec, date(2025, 10, 1), md)
    assert m.value is None and "matured" in m.note


def test_swaption_parity_and_expiry(monkeypatch):
    md = _md()
    monkeypatch.setattr(
        md.rates_history,
        "vols",
        lambda ccy, obs: [(1.0, 5.0, 0.01), (2.0, 10.0, 0.009)],
    )
    expiry = date(2025, 10, 1)
    terms = {
        **_draft("swaption", years=6),
        "start": expiry.isoformat(),
        "fixed_rate": 0.045,
    }
    payer = RateFxSpec.model_validate(terms)
    receiver = payer.model_copy(update={"payer": False})
    swap = payer.model_copy(update={"product": "swap"})
    d = date(2025, 2, 3)
    p, r = pr.mark(payer, d, md), pr.mark(receiver, d, md)
    assert p.value > 0 and r.value > 0
    assert p.value - r.value == pytest.approx(
        # The pricer counts whole years from the expiry, the swap its dates.
        pr.swap_value(swap, d, md.rates_history)[0],
        rel=2e-3,
    )
    assert "1Y into 5Y" in p.inputs[2].name
    assert pr.mark(payer, expiry, md).value is None


def test_a_swaption_without_traded_vols_has_no_mark():
    md = _md()
    spec = RateFxSpec.model_validate(
        {**_draft("swaption", years=6), "start": "2025-10-01", "fixed_rate": 0.04}
    )
    m = pr.mark(spec, START, md)
    assert m.value is None and "no volatility" in m.note


def test_a_swap_gains_its_annuity_per_basis_point_and_the_other_side_loses_it():
    md = _md()
    spec = pr.at_market(_draft(), START, md)
    g = pr.mark(spec, START, md).greeks
    _, times, dfs = md.rates_history.curve("USD", START)
    # Every zero rate one basis point up: each flow of the swap, known or
    # forward, loses its time × its value. For a payer at par,
    # d(1 − P(T) − K Σ P(t_i)) = (T P(T) + K Σ t_i P(t_i)) dz.
    pay = [(pr.years(START, p), p) for p in pr.schedule(spec)]
    weighted = sum(t * pr.discount(times, dfs, t) for t, _ in pay)
    last = pay[-1][0]
    expected = (
        spec.notional
        * 1e-4
        * (last * pr.discount(times, dfs, last) + spec.fixed_rate * weighted)
    )
    assert set(g) == {"pv01_usd"}
    assert g["pv01_usd"] == pytest.approx(expected, rel=1e-3)
    other = pr.mark(spec.model_copy(update={"payer": False}), START, md).greeks
    assert other["pv01_usd"] == pytest.approx(-g["pv01_usd"])
    # A coupon fixed on an earlier curve does not move with today's: once the
    # swap has started, less of it is left to move.
    later = pr.mark(spec, date(2025, 11, 14), md).greeks
    assert 0 < later["pv01_usd"] < g["pv01_usd"]


def test_an_fx_forward_is_long_its_euros_and_both_curves():
    md = _md()
    spec = pr.at_market(_draft("fx_forward", "EUR", 2), START, md)
    g = pr.mark(spec, START, md).greeks
    _, times, dfs = md.rates_history.curve("EUR", START)
    euros = spec.notional * pr.discount(times, dfs, pr.years(START, spec.maturity))
    assert g["fx_delta"] == pytest.approx(euros, rel=1e-9)
    # It pays dollars later (worth less when dollar rates rise: a gain) and
    # receives euros later (worth less when euro rates rise: a loss).
    assert g["pv01_usd"] > 0 > g["pv01_eur"]
    assert "normal_vega" not in g


def test_a_swaption_has_a_vega_and_the_position_carries_its_sensitivities(monkeypatch):
    md = _md()
    monkeypatch.setattr(md.rates_history, "vols", lambda ccy, obs: [(1.0, 5.0, 0.01)])
    spec = RateFxSpec.model_validate(
        {**_draft("swaption", years=6), "start": "2025-10-01", "fixed_rate": 0.045}
    )
    g = pr.mark(spec, date(2025, 2, 3), md).greeks
    assert g["normal_vega"] > 0 and g["pv01_usd"] > 0  # a payer: long rates

    swap = pr.at_market(_draft(), START, _md())
    [position] = pv.snapshot(_book(swap, quantity=-1.0), START).positions
    # Per contract: the page multiplies by the quantity held.
    assert position.greeks["pv01_usd"] > 0 and position.quantity == -1.0


VOLS = [(1.0, 2.0, 0.0117), (1.0, 5.0, 0.0111), (1.0, 10.0, 0.0103), (2.0, 5.0, 0.0114)]


def _bermudan(first="2025-10-01", end="2031-10-01", **more) -> RateFxSpec:
    return RateFxSpec.model_validate(
        {
            **_draft("bermudan"),
            "start": first,
            "maturity": end,
            "fixed_rate": 0.042,
            **more,
        }
    )


def test_a_bermudan_is_worth_at_least_its_first_european_and_exactly_it_with_one_date(
    monkeypatch,
):
    md = _md()
    monkeypatch.setattr(md.rates_history, "vols", lambda ccy, obs: VOLS)
    d = date(2025, 2, 3)
    m = pr.mark(_bermudan(), d, md)
    european = next(p.value for p in m.model.params if p.name.startswith("European"))
    # The right to wait is worth something: six exercise dates against one.
    assert m.value > european > 0
    assert m.model.model == "Hull-White, one factor" and m.inputs[-1].value == 6.0
    # One exercise date: a European swaption, to the lattice's accuracy.
    one = pr.mark(_bermudan(end="2026-10-01"), d, md)
    alone = next(p.value for p in one.model.params if p.name.startswith("European"))
    assert one.value == pytest.approx(alone, rel=2e-2)
    # A payer gains when rates rise; its PV01 is at fixed model parameters.
    assert m.greeks["pv01_usd"] > 0 and "normal_vega" not in m.greeks
    assert "Bermudan payer swaption USD 10M" in pr.label(_bermudan())


def test_a_bermudan_lives_on_its_later_dates_and_needs_enough_vols(monkeypatch):
    md = _md()
    monkeypatch.setattr(md.rates_history, "vols", lambda ccy, obs: VOLS)
    spec = _bermudan()
    # Its first date has passed: five are left.
    later = pr.mark(spec, date(2025, 11, 14), md)
    assert later.value > 0 and later.inputs[-1].value == 5.0
    assert pr.bermudan_exercises(spec, date(2025, 11, 14))[0] == date(2026, 10, 1)
    # ...until the last one, a year before the end of the swap.
    assert spec.end == date(2030, 10, 1)
    gone = pr.mark(spec, date(2030, 10, 1), md)
    assert gone.value is None and "last exercise date" in gone.note
    # Two vol points do not calibrate two parameters.
    monkeypatch.setattr(md.rates_history, "vols", lambda ccy, obs: VOLS[:2])
    md.rates_history._hull_white.clear()
    short = pr.mark(spec, date(2025, 2, 4), md)
    assert short.value is None and "not enough to calibrate" in short.note


def test_contracts_the_engine_cannot_hold_are_refused():
    with pytest.raises(ValueError, match="whole number of years"):
        RateFxSpec.model_validate(
            {**_draft(), "maturity": "2027-04-01", "fixed_rate": 0}
        )
    with pytest.raises(ValueError, match="euros against dollars"):
        RateFxSpec.model_validate({**_draft("fx_forward", "USD", 1), "fixed_rate": 1.1})
    with pytest.raises(ValueError, match="EUR coupon"):
        RateFxSpec.model_validate(
            {**_draft("cross_currency_swap", "EUR", 2), "fixed_rate": 0.04}
        )


def _book(spec: RateFxSpec, quantity=1.0, price=0.0, on=START) -> Portfolio:
    return Portfolio(
        id="p",
        version=2,
        base_currency="USD",
        instruments=[Instrument(id="irs", label=pr.label(spec), spec=spec)],
        transactions=[
            Trade(
                id="1",
                instrument_id="irs",
                trade_date=on,
                quantity=quantity,
                price=price,
            )
        ],
    )


def test_the_daily_pnl_of_a_swap_sums_to_its_mark():
    md = _md()
    # Traded after the days without a curve: a day with no mark has no P&L.
    on = date(2025, 4, 1)
    spec = pr.at_market(
        {**_draft(), "start": str(on), "maturity": "2030-04-01"}, on, md
    )
    pf = _book(spec, quantity=-1.0, on=on)
    end = date(2026, 6, 30)  # after the first coupon
    points, _ = pv.history(pf, on - timedelta(days=3), end)
    snap = pv.snapshot(pf, end)
    [position] = snap.positions
    assert position.kind == "rate_fx" and position.currency == "USD"
    assert position.mark == pytest.approx(pr.mark(spec, end, md).value)
    assert position.market_value == pytest.approx(-position.mark)
    assert all(p.daily_pnl is not None for p in points if p.date >= on)
    total = sum(p.daily_pnl for p in points if p.daily_pnl is not None)
    assert total == pytest.approx(snap.total_pnl, abs=1e-6 * spec.notional)
    assert snap.total_pnl == pytest.approx(-position.mark)


def test_the_quote_endpoint_fills_the_rates_and_refuses_a_day_without_market():
    client = TestClient(app)
    body = {"draft": _draft("cross_currency_swap", "EUR", 5), "trade_date": str(START)}
    r = client.post("/api/portfolio-valuation/rate-fx/quote", json=body)
    assert r.status_code == 200, r.text
    q = r.json()
    assert q["currency"] == "USD" and abs(q["value"]) < 10.0
    assert q["spec"]["domestic_notional"] == pytest.approx(1e7 * EURUSD[START])
    assert 0.03 < q["spec"]["fixed_rate"] < 0.05 and "Cross-currency" in q["label"]

    r = client.post(
        "/api/portfolio-valuation/rate-fx/quote",
        json={"draft": _draft(), "trade_date": str(GAP[-1])},
    )
    assert r.status_code == 503 and "no USD swap curve" in r.text

    bad = {"draft": {**_draft(), "maturity": "2027-04-01"}, "trade_date": str(START)}
    assert (
        client.post("/api/portfolio-valuation/rate-fx/quote", json=bad).status_code
        == 422
    )


def test_a_demo_with_rate_and_fx_trades_is_struck_at_the_market_and_faces_counterparties():
    from api.app import portfolio_demos as demos
    from api.app import portfolio_ledger

    on = date(2025, 6, 2)
    demo = demos.Demo(
        id="demo-test-otc",
        name="Test",
        description="",
        base_currency="USD",
        rate_fx=(
            demos.RateFx("swap", "swap", "USD", 10e6, True, 5, "example-dealer-vm"),
            demos.RateFx(
                "forward", "fx_forward", "EUR", 4e6, True, 2, "example-sovereign-isda"
            ),
            demos.RateFx(
                "ccs", "cross_currency_swap", "EUR", 6e6, False, 5,
                "example-sovereign-isda",
            ),
        ),
        trades=(
            demos.Buy(on, "swap", 1),
            demos.Buy(on, "forward", 1),
            demos.Buy(date(2025, 7, 1), "ccs", -1),
        ),
    )  # fmt: skip
    pf = demos.build(demo, pv.MarketData(on))
    portfolio_ledger.validate(pf)
    by_id = {i.id: i for i in pf.instruments}
    # Struck at the market of its own trade date: nothing changes hands.
    assert all(abs(t.price) < 1.0 for t in pf.transactions)
    assert by_id["ccs"].spec.start == date(2025, 7, 1)
    assert by_id["swap"].netting_set_id == "example-dealer-vm"
    assert "Payer swap USD 10M" in by_id["swap"].label
    assert {c.id for c in pf.counterparties} >= {"example-dealer", "example-sovereign"}
    # Valued like any portfolio, a year on.
    snap = pv.snapshot(pf, date(2026, 6, 2))
    assert [p.kind for p in snap.positions] == ["rate_fx"] * 3
    assert all(p.mark is not None for p in snap.positions)

    # No swaption traded in the store: the demo is unavailable, not guessed.
    option = demos.RateFx(
        "swaption", "swaption", "USD", 5e6, True, 5, "example-dealer-vm", 1
    )
    bad = demos.Demo(
        id="demo-test-swaption",
        name="Test",
        description="",
        base_currency="USD",
        rate_fx=(option,),
        trades=(demos.Buy(on, "swaption", 1),),
    )
    with pytest.raises(LookupError, match="no volatility"):
        demos.build(bad, pv.MarketData(on))
