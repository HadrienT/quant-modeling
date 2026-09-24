"""Portfolio storage round-trip on the real JSON backend: a ledger (dates in
trades and derivative expiries) is written and read back unchanged."""

from datetime import date

from api.app import portfolio_storage
from api.app.portfolio_schemas import (
    DerivativeSpec,
    EquitySpec,
    Instrument,
    Portfolio,
    Trade,
)
from api.app.storage import LocalJsonStorage


def test_a_ledger_with_dates_is_saved_and_read_back(tmp_path, monkeypatch):
    store = LocalJsonStorage(tmp_path)
    monkeypatch.setattr(portfolio_storage, "get_storage", lambda: store)
    pf = Portfolio(
        id="p1",
        owner="alice",
        version=2,
        instruments=[
            Instrument(id="EQ:MC.PA", spec=EquitySpec(ticker="MC.PA")),
            Instrument(
                id="call",
                spec=DerivativeSpec(
                    product="vanilla",
                    underlying="^FCHI",
                    expiry=date(2027, 3, 19),
                    params={"strike": 8200.0, "is_call": True},
                ),
            ),
        ],
        transactions=[
            Trade(
                id="t1",
                instrument_id="EQ:MC.PA",
                trade_date=date(2026, 9, 24),
                quantity=10,
                price=397.1,
            ),
            Trade(
                id="t2",
                instrument_id="call",
                trade_date=date(2026, 9, 24),
                quantity=-2,
                price=205.0,
            ),
        ],
    )
    portfolio_storage.save_portfolio(pf)
    back = portfolio_storage.get_portfolio("p1", "alice")
    assert back == pf
    [summary] = portfolio_storage.list_portfolios("alice")
    assert summary.n_positions == 2
