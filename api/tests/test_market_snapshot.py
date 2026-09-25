"""Market data for scripted local-vol pricing comes from the database, never
from Yahoo. These tests replace the database and the native calibrator with
fakes, so they exercise the rules (what is refused, what is reported, which
date is priced) without a Postgres or a built module."""

import subprocess
import sys
import types
from datetime import date

import pytest

from app import db, market_snapshot as ms

SNAP = date(2026, 9, 12)  # a Saturday, like the real stored snapshot


class Row:
    def __init__(self, expiry, strike, kind):
        self.snapshot_date, self.expiry, self.option_type = SNAP, expiry, kind
        self.strike, self.bid, self.ask, self.last_price = strike, 1.0, 1.2, 1.1
        self.volume, self.open_interest, self.implied_volatility = 10, 100, 0.2


class FakeQuote:
    pass


@pytest.fixture
def fake(monkeypatch):
    """A healthy store: snapshot on SNAP, close and yield on the same day."""
    state = dict(
        snap=SNAP,
        price=(SNAP, 500.0),
        div=(SNAP, 0.01),
        rows=[Row(date(2027, 1, 15), 500.0, "call"), Row(date(2027, 1, 15), 500.0, "put")],
        down=False,
        calls=[],
    )

    def guard(v):
        if state["down"]:
            raise db.StoreUnavailable("connection refused")
        return v

    monkeypatch.setattr(db, "options_snapshot_date_on_or_before",
                        lambda t, d: guard(state["snap"]))
    monkeypatch.setattr(db, "price_on_or_before", lambda t, d: guard(state["price"]))
    monkeypatch.setattr(db, "dividend_yield_on_or_before", lambda t, d: guard(state["div"]))
    monkeypatch.setattr(db, "options_chain_snapshot", lambda t, d: guard(state["rows"]))

    def calibrate(quotes, spot, rate, dividend, *a, **k):
        state["calls"].append((len(quotes), spot, rate, dividend))
        return {"K_grid": [90.0, 110.0], "T_grid": [0.1, 1.0],
                "sigma_loc_flat": [0.2, 0.2, 0.2, 0.2]}

    monkeypatch.setattr(ms, "qm", types.SimpleNamespace(
        RawOptionQuote=FakeQuote, CleaningParams=lambda: None,
        calibrate_vol_surface=calibrate))
    return state


def test_healthy_store_prices_on_the_snapshot_date(fake):
    m = ms.local_vol_market("spy", 0.03, SNAP)
    assert m.valuation_date == SNAP and m.spot == 500.0 and m.dividend == 0.01
    assert m.warnings == []
    assert fake["calls"] == [(2, 500.0, 0.03, 0.01)]


def test_a_later_valuation_date_is_priced_on_the_stored_market_date(fake):
    m = ms.local_vol_market("SPY", 0.03, date(2026, 9, 14))  # Monday
    assert m.valuation_date == SNAP
    assert [w["code"] for w in m.warnings] == ["market_date_shifted"]


def test_a_ticker_with_no_snapshot_is_refused_and_names_the_universe(fake):
    fake["snap"] = None
    with pytest.raises(ms.MarketDataUnavailable, match="OPTIONS_CHAIN_TICKERS"):
        ms.local_vol_market("ZZZZ", 0.03, SNAP)


def test_a_snapshot_too_old_for_the_valuation_date_says_how_to_proceed(fake):
    with pytest.raises(ms.MarketDataUnavailable) as e:
        ms.local_vol_market("SPY", 0.03, date(2026, 9, 19))
    msg = str(e.value)
    assert "2026-09-12" in msg and "valuation_date=2026-09-12" in msg


def test_missing_close_is_refused(fake):
    fake["price"] = None
    with pytest.raises(ms.MarketDataUnavailable, match="sp500-prices"):
        ms.local_vol_market("SPY", 0.03, SNAP)


def test_a_close_older_than_the_limit_is_refused(fake):
    fake["price"] = (date(2026, 9, 1), 500.0)
    with pytest.raises(ms.MarketDataUnavailable, match="latest is 2026-09-01"):
        ms.local_vol_market("SPY", 0.03, SNAP)


def test_a_close_a_few_days_behind_the_chain_is_accepted_but_reported(fake):
    fake["price"] = (date(2026, 9, 7), 500.0)  # what the real database holds
    m = ms.local_vol_market("SPY", 0.03, SNAP)
    stale = [w for w in m.warnings if w["code"] == "stale_spot"]
    assert stale and stale[0]["severity"] == "warning"
    assert "2026-09-07" in stale[0]["message"]


def test_missing_dividend_yield_means_never_ingested_not_zero(fake):
    fake["div"] = None
    with pytest.raises(ms.MarketDataUnavailable, match="never"):
        ms.local_vol_market("NFLX", 0.03, SNAP)


def test_a_stored_zero_yield_is_a_valid_non_payer(fake):
    fake["div"] = (SNAP, 0.0)
    assert ms.local_vol_market("TSLA", 0.03, SNAP).dividend == 0.0


def test_an_unreachable_database_is_reported_not_papered_over(fake):
    fake["down"] = True
    with pytest.raises(ms.MarketDataUnavailable, match="no live fallback"):
        ms.local_vol_market("SPY", 0.03, SNAP)


def test_the_module_never_loads_yfinance():
    """Structural, not conventional: importing the market-data path must not
    even import yfinance, so no code path here can reach Yahoo."""
    out = subprocess.run(
        [sys.executable, "-c",
         "import sys; sys.path.insert(0, 'api'); import app.market_snapshot; "
         "print('yfinance' in sys.modules)"],
        capture_output=True, text=True,
    )
    assert out.stdout.strip() == "False", out.stderr


def test_the_service_prices_on_the_snapshot_date_and_merges_the_warnings(fake, monkeypatch):
    from app import pricing_service
    from app.schemas import ScriptRequest

    seen = {}

    def fake_price_script(*args, **kwargs):
        seen["args"] = args
        return {"npv": 1.0, "greeks": {}, "diagnostics": "d", "mc_std_error": 0.0,
                "warnings": [{"code": "forward_smile", "severity": "info", "message": "m"}]}

    monkeypatch.setattr(pricing_service.qm, "price_script", fake_price_script, raising=False)
    fake["price"] = (date(2026, 9, 7), 500.0)

    resp = pricing_service.price_script(ScriptRequest(
        script="x", rate=0.03, model="local_vol", ticker="SPY",
        valuation_date=date(2026, 9, 14)))

    assert seen["args"][5] == "2026-09-12"  # valuation date handed to the engine
    assert [w.code for w in resp.warnings] == ["market_date_shifted", "stale_spot", "forward_smile"]


def _collected(run):
    """Runs `run` under the valuation record's market-inputs collector."""
    from app import valuation

    inputs = []
    token = valuation._market_inputs.set(inputs)
    try:
        run()
    finally:
        valuation._market_inputs.reset(token)
    return {i.name: i for i in inputs}


def test_the_valuation_record_gets_every_input_read_with_its_date(fake):
    inputs = _collected(lambda: ms.local_vol_market("spy", 0.03, SNAP))
    assert set(inputs) == {"spot:SPY", "dividend:SPY", "option_chain:SPY"}
    assert inputs["spot:SPY"].source == "db:prices.sp500_daily"
    assert inputs["option_chain:SPY"].as_of == SNAP.isoformat()
    assert all(i.status.value == "observed" for i in inputs.values())


def test_a_close_behind_the_chain_is_recorded_as_stale(fake):
    fake["price"] = (date(2026, 9, 7), 500.0)
    inputs = _collected(lambda: ms.local_vol_market("SPY", 0.03, SNAP))
    assert inputs["spot:SPY"].status.value == "stale"
    assert inputs["spot:SPY"].as_of == "2026-09-07"


def test_a_revised_quote_changes_the_chain_hash(fake):
    before = _collected(lambda: ms.local_vol_market("SPY", 0.03, SNAP))
    fake["rows"][0].bid = 1.05
    after = _collected(lambda: ms.local_vol_market("SPY", 0.03, SNAP))
    assert before["option_chain:SPY"].value_hash != after["option_chain:SPY"].value_hash
    assert before["spot:SPY"].value_hash == after["spot:SPY"].value_hash
