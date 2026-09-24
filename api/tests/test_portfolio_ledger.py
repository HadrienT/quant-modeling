"""Portfolio ledger: weighted-average-cost accounting, shorts, migration."""

from __future__ import annotations

from datetime import date

import pytest

from api.app import portfolio_ledger as L
from api.app.portfolio_schemas import (
    EquitySpec,
    Instrument,
    Portfolio,
    Position,
    Trade,
)

D = date(2026, 9, 1)


def _pf(*trades: tuple) -> Portfolio:
    """trades: (quantity, price, fees, day offset)"""
    return Portfolio(
        id="p",
        version=2,
        instruments=[Instrument(id="EQ:AAA", spec=EquitySpec(ticker="AAA"))],
        transactions=[
            Trade(
                id=f"t{i}",
                instrument_id="EQ:AAA",
                trade_date=date(2026, 9, 1 + d),
                quantity=q,
                price=p,
                fees=f,
                created_at=f"2026-09-01T00:00:{i:02d}",
            )
            for i, (q, p, f, d) in enumerate(trades)
        ],
    )


def test_buys_average_the_cost():
    h = L.holdings(_pf((100, 10.0, 0, 0), (100, 20.0, 0, 1)))["EQ:AAA"]
    assert h.quantity == 200 and h.average_cost == pytest.approx(15.0)
    assert h.realised == 0.0


def test_a_partial_sale_realises_against_the_average_and_keeps_it():
    h = L.holdings(_pf((100, 10.0, 0, 0), (100, 20.0, 0, 1), (-50, 18.0, 0, 2)))[
        "EQ:AAA"
    ]
    assert h.quantity == 150
    assert h.average_cost == pytest.approx(15.0)  # PMP unchanged by a sale
    assert h.realised == pytest.approx(50 * (18.0 - 15.0))


def test_selling_more_than_held_opens_a_short_at_the_sale_price():
    h = L.holdings(_pf((100, 10.0, 0, 0), (-150, 12.0, 0, 1)))["EQ:AAA"]
    assert h.quantity == -50 and h.average_cost == 12.0
    assert h.realised == pytest.approx(100 * 2.0)


def test_a_short_is_closed_by_buying_back():
    h = L.holdings(_pf((-100, 20.0, 0, 0), (100, 15.0, 0, 1)))["EQ:AAA"]
    assert h.quantity == 0 and h.average_cost == 0
    assert h.realised == pytest.approx(
        100 * (20.0 - 15.0)
    )  # short gains when price falls


def test_fees_are_charged_to_realised_pnl():
    h = L.holdings(_pf((100, 10.0, 2.5, 0), (-100, 10.0, 2.5, 1)))["EQ:AAA"]
    assert h.realised == pytest.approx(-5.0) and h.fees == pytest.approx(5.0)


def test_holdings_as_of_a_date_ignore_later_trades():
    pf = _pf((100, 10.0, 0, 0), (-40, 11.0, 0, 5))
    assert L.holdings(pf, as_of=date(2026, 9, 3))["EQ:AAA"].quantity == 100
    assert L.holdings(pf)["EQ:AAA"].quantity == 60


def test_realised_plus_unrealised_is_the_cash_view():
    """PMP is an allocation of P&L, not a different P&L: at any mark,
    realised + unrealised = market value + sale proceeds − purchase costs − fees."""
    trades = [
        (100, 10.0, 1.0, 0),
        (50, 14.0, 1.0, 1),
        (-120, 13.0, 1.0, 2),
        (-60, 9.0, 1.0, 3),
        (40, 11.0, 1.0, 4),
    ]
    h = L.holdings(_pf(*trades))["EQ:AAA"]
    mark = 12.3
    unrealised = (mark - h.average_cost) * h.quantity
    cash = -sum(q * p for q, p, _, _ in trades) - sum(f for _, _, f, _ in trades)
    assert h.realised + unrealised == pytest.approx(h.quantity * mark + cash, abs=1e-9)


def test_unknown_instrument_is_refused():
    pf = _pf((1, 1.0, 0, 0))
    pf.instruments = []
    with pytest.raises(ValueError, match="unknown instruments"):
        L.validate(pf)


def test_migration_turns_positions_into_one_trade_each_and_converts_front_percents():
    legacy = Portfolio(
        id="old",
        created_at="2026-01-10T12:00:00",
        positions=[
            Position(
                id="a",
                label="Call",
                product_type="vanilla",
                direction="short",
                quantity=3,
                entry_price=9.5,
                parameters={
                    "spot": 100,
                    "strike": 100,
                    "maturity": 1,
                    "rate": 4,
                    "dividend": 0,
                    "vol": 20,
                    "is_call": True,
                },
            ),
            Position(
                id="b",
                label="Put",
                product_type="european-put",
                quantity=2,
                entry_price=5,
                parameters={
                    "spot": 100,
                    "strike": 95,
                    "maturity": 0.5,
                    "rate": 0.04,
                    "dividend": 0.0,
                    "vol": 0.2,
                },
            ),
            Position(id="c", label="Mystery", product_type="teleport", quantity=1),
        ],
    )
    pf = L.migrate(legacy)
    assert pf.version == 2
    by_id = {i.id: i for i in pf.instruments}
    assert by_id["a"].spec.params["vol"] == pytest.approx(
        0.20
    )  # front form value 20 → 0.20
    assert by_id["b"].spec.params["vol"] == pytest.approx(0.20)  # API value kept
    assert by_id["b"].spec.params["is_call"] is False
    assert by_id["a"].spec.expiry == date(2027, 1, 10)
    trades = {t.instrument_id: t for t in pf.transactions}
    assert trades["a"].quantity == -3 and trades["a"].price == 9.5
    assert [p.id for p in pf.positions] == ["c"]  # unknown: kept, never dropped
    assert L.migrate(pf) == pf  # idempotent
