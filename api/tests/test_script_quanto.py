"""Quantos and composites in scripts (issue #86): an asset listed in another
currency than the payment one is simulated under the payment currency's
measure, and an exchange rate can be an underlying. Properties with closed
forms: a composite call is Black-Scholes on S*X at the combined vol, and a
quanto forward grows at r_f - q - rho sigma_S sigma_X. The database is a
fake: a EUR stock, a USD stock and the EUR/USD fixings, correlated."""

import math
from datetime import date, timedelta

import numpy as np
import pandas as pd
import pytest
from pydantic import ValidationError

from app import db, fx, multi_asset_market, pricing_service, vol_smile
from app.schemas import ScriptRequest

D = date(2026, 9, 11)
PAY = date(2027, 9, 11)
T = (PAY - D).days / 365.0
R_USD, R_EUR, Q = 0.04, 0.02, 0.01


def _n(x: float) -> float:
    return 0.5 * math.erfc(-x / math.sqrt(2.0))


def _bs_call(S, K, T, r, q, vol):
    d1 = (math.log(S / K) + (r - q + 0.5 * vol * vol) * T) / (vol * math.sqrt(T))
    d2 = d1 - vol * math.sqrt(T)
    return S * math.exp(-q * T) * _n(d1) - K * math.exp(-r * T) * _n(d2)


@pytest.fixture
def store(monkeypatch):
    rng = np.random.default_rng(3)
    days = pd.bdate_range(D - timedelta(days=4 * 365), D)
    z = rng.standard_normal((len(days), 3))
    z[:, 2] = -0.4 * z[:, 0] + math.sqrt(1 - 0.16) * z[:, 2]  # EURUSD vs AAA
    series = {
        "AAA": 80 * np.exp(np.cumsum(0.014 * z[:, 0])),  # EUR
        "BBB": 120 * np.exp(np.cumsum(0.018 * z[:, 1])),  # USD
        "EURUSD": 1.1 * np.exp(np.cumsum(0.006 * z[:, 2])),
    }
    wide = pd.DataFrame(series, index=days)
    ccy = {"AAA": "EUR", "BBB": "USD"}

    def history(t, since):
        return [(d.date(), float(v)) for d, v in wide[t].items() if d.date() >= since]

    def pair(base, quote, since=None):
        if (base, quote) != ("EUR", "USD"):
            raise fx.FxUnavailable(f"no {base}/{quote} in the fake store")
        s = wide["EURUSD"]
        return s[s.index.date >= since] if since else s

    monkeypatch.setattr(db, "ticker_currency", lambda t: ccy[t])
    monkeypatch.setattr(
        db,
        "price_on_or_before",
        lambda t, d: (days[-1].date(), float(wide[t].iloc[-1])),
    )
    monkeypatch.setattr(db, "dividend_yield_on_or_before", lambda t, d: (D, Q))
    monkeypatch.setattr(db, "price_history", history)
    monkeypatch.setattr(fx, "pair_history", pair)
    monkeypatch.setattr(vol_smile, "smile_for", lambda *a, **k: None)
    monkeypatch.setattr(
        multi_asset_market,
        "_foreign_rate",
        lambda c, d, h: {"EUR": R_EUR, "USD": R_USD}[c],
    )
    return wide


def _req(script, underlyings, **kw):
    base = dict(
        script=script,
        rate=R_USD,
        valuation_date=D,
        n_paths=200_000,
        seed=11,
        underlyings=underlyings,
    )
    return ScriptRequest(**{**base, **kw})


def test_a_composite_call_is_black_scholes_on_the_converted_asset(store):
    script = f"{PAY.isoformat()}\n    pays max(spot(0) * spot(1) - 90, 0)\n"
    resp = pricing_service.price_script(
        _req(script, [{"ticker": "AAA"}, {"fx": "EUR"}], currency="USD")
    )
    s, x = resp.model_choice.underlyings
    rho = resp.model_choice.correlation[0][1]
    assert (s.currency, x.fx, x.currency) == ("EUR", "EUR", "USD")
    assert x.drift_adjustment == pytest.approx(R_EUR)  # X drifts at r_d - r_f
    vol = math.sqrt(s.vol**2 + x.vol**2 + 2 * rho * s.vol * x.vol)
    exact = _bs_call(s.spot * x.spot, 90.0, T, R_USD, Q, vol)
    assert resp.npv == pytest.approx(exact, abs=4 * resp.mc_std_error)
    assert rho < -0.2  # the fake's -0.4, estimated on weekly returns


def test_a_quanto_forward_grows_at_the_quanto_drift(store):
    # spot(1), the EUR/USD rate, is read with no weight: it is an underlying
    # for its correlation, and the payoff is the quanto's.
    script = f"{PAY.isoformat()}\n    pays spot(0) + 0 * spot(1)\n"
    resp = pricing_service.price_script(
        _req(script, [{"ticker": "AAA"}, {"fx": "EUR"}], currency="USD")
    )
    s, x = resp.model_choice.underlyings
    rho = resp.model_choice.correlation[0][1]
    assert s.drift_adjustment == pytest.approx(
        R_USD - R_EUR + rho * s.vol * x.vol, rel=1e-12
    )
    exact = s.spot * math.exp((R_EUR - Q - rho * s.vol * x.vol) * T - R_USD * T)
    assert resp.npv == pytest.approx(exact, abs=4 * resp.mc_std_error)
    assert "quanto_drift" in {w.code for w in resp.warnings}


def test_one_foreign_asset_alone_is_a_quanto(store):
    # The EUR/USD rate is not an underlying: its vol and its correlation to
    # the asset are still read, for the drift.
    script = f"{PAY.isoformat()}\n    pays spot()\n"
    resp = pricing_service.price_script(
        _req(script, [{"ticker": "AAA"}], currency="USD")
    )
    [s] = resp.model_choice.underlyings
    assert s.drift_adjustment != 0.0 and "rho" in s.drift_source
    exact = s.spot * math.exp(-(Q + s.drift_adjustment) * T)
    assert resp.npv == pytest.approx(exact, abs=4 * resp.mc_std_error)


def test_assets_in_the_payment_currency_are_not_adjusted(store):
    script = f"{PAY.isoformat()}\n    pays spot(0) + spot(1)\n"
    resp = pricing_service.price_script(
        _req(script, [{"ticker": "BBB"}, {"ticker": "AAA"}])  # pays in USD, BBB's
    )
    b, a = resp.model_choice.underlyings
    assert b.drift_adjustment == 0.0 and a.drift_adjustment != 0.0
    assert "weekly" in resp.model_choice.correlation_source


@pytest.mark.parametrize(
    "underlyings, extra, match",
    [
        ([{"ticker": "AAA", "fx": "EUR"}], {}, "not both"),
        (
            [{"spot": 1.0, "vol": 0.2}, {"spot": 1.0, "vol": 0.2}],
            {"currency": "EUR"},
            "typed",
        ),
    ],
)
def test_malformed_requests_are_refused(underlyings, extra, match):
    with pytest.raises(ValidationError, match=match):
        _req(
            "2027-09-11\n    pays spot()\n",
            underlyings,
            correlation=[[1, 0], [0, 1]],
            **extra,
        )


def test_an_exchange_rate_in_its_own_currency_is_refused(store):
    with pytest.raises(ValueError, match="payment currency"):
        pricing_service.price_script(
            _req(
                f"{PAY.isoformat()}\n    pays spot(0)\n",
                [{"fx": "USD"}],
                currency="USD",
            )
        )
