"""Scripts on several underlyings (spot(0), spot(1)...): correlated
Black-Scholes, inputs typed or read from the database. The native module
prices for real; the database is a fake with two correlated price histories."""

from datetime import date, timedelta

import numpy as np
import pandas as pd
import pytest
from pydantic import ValidationError

from app import db, multi_asset_market, pricing_service, valuation, vol_smile
from app.schemas import ScriptRequest

D = date(2026, 9, 11)
WORST_OF = (
    "2026-12-10\n    s0 = spot(0)\n    s1 = spot(1)\n\n"
    "2027-09-10\n    pays 1000 * max(min(spot(0) / s0, spot(1) / s1) - 1, 0)\n"
)
TYPED = [dict(spot=100.0, vol=0.25), dict(spot=50.0, vol=0.3, dividend=0.01)]


def _req(**kw):
    base = dict(script=WORST_OF, rate=0.03, valuation_date=D, n_paths=4000)
    return ScriptRequest(**{**base, **kw})


@pytest.fixture
def store(monkeypatch):
    """Two stocks whose daily log returns have correlation ~0.6, no option
    chain for either (so their vols are realised-vol proxies)."""
    rng = np.random.default_rng(0)
    days = pd.bdate_range(D - timedelta(days=400), D)
    z = rng.standard_normal((len(days), 2))
    z[:, 1] = 0.6 * z[:, 0] + 0.8 * z[:, 1]
    closes = {
        "AAA": 100 * np.exp(np.cumsum(0.015 * z[:, 0])),
        "BBB": 50 * np.exp(np.cumsum(0.02 * z[:, 1])),
    }
    wide = pd.DataFrame(closes, index=days.tz_localize("UTC"))

    def history(t, since):
        return [(d.date(), float(v)) for d, v in wide[t].items() if d.date() >= since]

    monkeypatch.setattr(
        db,
        "price_on_or_before",
        lambda t, d: (days[-1].date(), float(wide[t].iloc[-1])),
    )
    monkeypatch.setattr(db, "dividend_yield_on_or_before", lambda t, d: (D, 0.01))
    monkeypatch.setattr(db, "ticker_currency", lambda t: "USD")
    monkeypatch.setattr(db, "price_history", history)
    monkeypatch.setattr(db, "prices_wide", lambda ts, since: wide[list(ts)])
    monkeypatch.setattr(vol_smile, "smile_for", lambda *a, **k: None)
    return wide


def test_typed_underlyings_price_under_correlated_black_scholes():
    resp = pricing_service.price_script(
        _req(underlyings=TYPED, correlation=[[1, 0.5], [0.5, 1]])
    )
    c = resp.model_choice
    assert (c.model, c.code) == ("black_scholes", "multi_asset")
    assert [u.vol for u in c.underlyings] == [0.25, 0.3]
    assert c.correlation_source == "typed"
    assert {"correlation", "flat_vol_smile"} <= {w.code for w in resp.warnings}
    assert "2 correlated assets" in resp.diagnostics
    assert resp.npv > 0


def test_a_worst_of_is_worth_more_when_its_assets_move_together():
    def price(rho):
        return pricing_service.price_script(
            _req(underlyings=TYPED, correlation=[[1, rho], [rho, 1]], seed=7)
        ).npv

    assert price(0.9) > price(0.0)


def test_tickers_read_spots_vols_and_historical_correlation(store):
    resp = pricing_service.price_script(
        _req(underlyings=[dict(ticker="AAA"), dict(ticker="bbb")])
    )
    c = resp.model_choice
    assert [u.ticker for u in c.underlyings] == ["AAA", "BBB"]
    assert all("realised" in u.vol_source for u in c.underlyings)
    # Daily moves of 1.5 % and 2 %: about 24 % and 32 % a year.
    assert c.underlyings[0].vol == pytest.approx(0.015 * np.sqrt(252), rel=0.15)
    assert c.correlation[0][1] == pytest.approx(0.6, abs=0.1)
    assert c.correlation_source.startswith("historical")
    assert [w.code for w in resp.warnings].count("vol_proxied") == 2
    assert "correlation_sparse_history" not in {w.code for w in resp.warnings}


def test_holes_in_the_stored_history_are_reported(store, monkeypatch):
    holed = store.drop(store.index[100:130])  # six weeks missing
    monkeypatch.setattr(db, "prices_wide", lambda ts, since: holed[list(ts)])
    resp = pricing_service.price_script(
        _req(underlyings=[dict(ticker="AAA"), dict(ticker="BBB")])
    )
    sparse = [w for w in resp.warnings if w.code == "correlation_sparse_history"]
    assert sparse and "1 gap(s)" in sparse[0].message


def test_the_valuation_record_lists_the_underlyings(store):
    req = _req(underlyings=[dict(ticker="AAA"), dict(ticker="BBB")])
    priced = valuation.price("script", req)
    names = {i.name for i in priced.market_inputs}
    assert {"spot:AAA", "vol:BBB", "correlation:AAA/BBB"} <= names
    spec = valuation.payload("script", req, priced, ip_hash=None).model
    assert spec.params["ticker[1]"] == "BBB"


def test_too_little_common_history_is_an_error_not_a_guess(store, monkeypatch):
    monkeypatch.setattr(db, "prices_wide", lambda ts, since: store[list(ts)].tail(10))
    with pytest.raises(multi_asset_market.MarketDataUnavailable, match="common daily"):
        pricing_service.price_script(
            _req(underlyings=[dict(ticker="AAA"), dict(ticker="BBB")])
        )


def test_the_number_of_underlyings_must_match_the_script():
    three = TYPED + [dict(spot=10.0, vol=0.2)]
    with pytest.raises(ValueError, match="reads 2 underlying"):
        pricing_service.price_script(
            _req(underlyings=three, correlation=np.eye(3).tolist())
        )


def test_malformed_multi_asset_requests_are_refused():
    with pytest.raises(ValidationError, match="correlation"):
        _req(underlyings=TYPED)
    with pytest.raises(ValidationError, match="every entry"):
        _req(underlyings=[dict(ticker="AAA"), dict(spot=1.0, vol=0.2)])
    with pytest.raises(ValidationError, match="single-underlying"):
        _req(underlyings=TYPED, correlation=[[1, 0], [0, 1]], model="slv")


# ── Multi-asset local vol (issue #86) ────────────────────────────────────────


def _surface(ticker, snap, level):
    from app import market_snapshot

    K = [40.0 + 10.0 * i for i in range(15)]
    T = [0.1, 0.5, 1.0, 2.0]
    return market_snapshot.LocalVolMarket(
        ticker=ticker,
        valuation_date=snap,
        spot=100.0 if ticker == "AAA" else 50.0,
        dividend=0.01,
        K_grid=K,
        T_grid=T,
        sigma_loc_flat=[level for _ in K for _ in T],
        rate=0.03,
    )


@pytest.fixture
def surfaces(store, monkeypatch):
    from app import market_snapshot

    snaps = {"AAA": D, "BBB": D}
    monkeypatch.setattr(
        market_snapshot,
        "local_vol_market",
        lambda t, r, d: _surface(t, snaps[t], 0.25 if t == "AAA" else 0.3),
    )
    return snaps


def test_every_stored_chain_gives_each_asset_its_own_local_vol(surfaces):
    resp = pricing_service.price_script(
        _req(underlyings=[{"ticker": "AAA"}, {"ticker": "BBB"}])
    )
    c = resp.model_choice
    assert (c.model, c.code) == ("local_vol", "multi_asset_local_vol")
    assert all("Dupire surface" in u.vol_source for u in c.underlyings)
    assert [u.spot for u in c.underlyings] == [100.0, 50.0]  # the surfaces' closes
    assert "one Dupire surface each" in resp.diagnostics
    assert "flat_vol_smile" not in {w.code for w in resp.warnings}


def test_chains_of_different_dates_fall_back_to_black_scholes_and_say_why(surfaces):
    surfaces["BBB"] = D - timedelta(days=1)
    resp = pricing_service.price_script(
        _req(underlyings=[{"ticker": "AAA"}, {"ticker": "BBB"}])
    )
    c = resp.model_choice
    assert (c.model, c.code) == ("black_scholes", "multi_asset")
    assert "different dates" in c.reason


def test_local_vol_asked_without_surfaces_is_refused(store):
    with pytest.raises(ValueError, match="several underlyings"):
        pricing_service.price_script(
            _req(underlyings=[{"ticker": "AAA"}, {"ticker": "BBB"}], model="local_vol")
        )
