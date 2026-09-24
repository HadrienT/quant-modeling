"""Equity markets — `/market/markets`, `/market/tickers?market=`, and the
currency on `/market/prices/history`, with the database replaced by a fake."""

from __future__ import annotations

import os
from datetime import date

os.environ.setdefault("JWT_SECRET", "test-only-secret-not-for-deployment")

import pytest
from fastapi.testclient import TestClient

from api.app import db
from api.app.main import app
from api.app.routers import market_price_tape as tape

AS_OF = date(2026, 9, 24)
MEMBERS = {
    "CAC40": [
        db.MarketMember("^FCHI", "CAC 40", "index", "EUR"),
        db.MarketMember("MC.PA", "LVMH", "equity", "EUR"),
    ],
    "FTSE100": [
        db.MarketMember("^FTSE", "FTSE 100", "index", "GBP"),
        db.MarketMember("AZN.L", "AstraZeneca", "equity", "GBP"),
    ],
    "SP500": [db.MarketMember("AAPL", None, "equity", "USD")],
}


@pytest.fixture
def client(monkeypatch):
    monkeypatch.setattr(
        db,
        "equity_markets",
        lambda: [(m, len(v) - 1, AS_OF) for m, v in MEMBERS.items()],
    )
    monkeypatch.setattr(db, "market_members", lambda m: MEMBERS.get(m, []))
    monkeypatch.setattr(db, "sp500_tickers", lambda: ["AAPL"])
    monkeypatch.setattr(
        db,
        "price_history",
        lambda t, since: [(date(2026, 9, 23), 124.0), (AS_OF, 124.36)],
    )
    currencies = {"AZN.L": "GBP", "MC.PA": "EUR"}
    monkeypatch.setattr(db, "ticker_currency", lambda t: currencies.get(t))
    for cache in (tape._MARKETS_CACHE, tape._TICKERS_CACHE, tape._HISTORY_CACHE):
        monkeypatch.setattr(
            tape, _name_of(cache), type(cache)(max_size=16, ttl_seconds=60)
        )
    return TestClient(app)


def _name_of(cache) -> str:
    return next(k for k, v in vars(tape).items() if v is cache)


def test_markets_say_which_have_option_data(client):
    markets = {m["id"]: m for m in client.get("/market/markets").json()["markets"]}
    assert markets["SP500"]["has_options"] is True
    assert markets["CAC40"]["has_options"] is False
    assert "US listings only" in markets["FTSE100"]["note"]
    assert "pence" in markets["FTSE100"]["note"]
    assert markets["CAC40"]["currency"] == "EUR" and markets["CAC40"]["members"] == 1


def test_a_market_lists_its_index_first_with_names_and_currency(client):
    body = client.get("/market/tickers?market=CAC40").json()
    assert body["tickers"] == ["^FCHI", "MC.PA"]
    assert body["members"][0] == {
        "ticker": "^FCHI",
        "name": "CAC 40",
        "kind": "index",
        "currency": "EUR",
    }


def test_sp500_keeps_its_index_etfs(client):
    tickers = client.get("/market/tickers?market=SP500").json()["tickers"]
    assert "AAPL" in tickers and "SPY" in tickers


def test_the_market_less_list_is_unchanged(client):
    body = client.get("/market/tickers").json()
    assert body["members"] == [] and "AAPL" in body["tickers"]


def test_an_unknown_market_is_rejected(client):
    assert client.get("/market/tickers?market=MARS").status_code == 422


def test_history_carries_the_currency_never_converted(client):
    body = client.get("/market/prices/history?ticker=AZN.L&range=1M").json()
    assert body["currency"] == "GBP" and body["points"][-1]["close"] == 124.36
    # Outside the universe (an index ETF): a US listing.
    assert (
        client.get("/market/prices/history?ticker=SPY&range=1M").json()["currency"]
        == "USD"
    )
