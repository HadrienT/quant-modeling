"""FX — `fx.py` and `/market/fx/*`, the database replaced by a fake.

Property tests: covered interest parity on real discount curves; a known
correlation is recovered, inside its confidence interval; asynchronous
sampling biases daily correlation towards zero and weekly returns undo it
(the Epps effect the methodology cites); realised volatility of a simulated
lognormal walk recovers its volatility.
"""

from __future__ import annotations

import math
import os
from datetime import date

os.environ.setdefault("JWT_SECRET", "test-only-secret-not-for-deployment")

import numpy as np
import pandas as pd
import pytest
from fastapi.testclient import TestClient

from api.app import db, fx, rates
from api.app.main import app
from api.app.routers import market_fx

DAYS = pd.bdate_range("2021-01-04", "2026-09-24")


def _walk(returns: np.ndarray, start: float = 1.0) -> pd.Series:
    return pd.Series(start * np.exp(np.cumsum(returns)), index=DAYS.date)


def _correlated(rho: float, seed: int = 1, vol_a: float = 0.2, vol_b: float = 0.08):
    rng = np.random.default_rng(seed)
    z = rng.standard_normal((len(DAYS), 2))
    a = z[:, 0]
    b = rho * z[:, 0] + math.sqrt(1 - rho**2) * z[:, 1]
    dt = 1 / 252
    return _walk(vol_a * math.sqrt(dt) * a, 100.0), _walk(
        vol_b * math.sqrt(dt) * b, 1.1
    )


def test_a_known_correlation_is_recovered_inside_its_interval():
    a, b = _correlated(0.4)
    c = fx.correlation(a, b, "daily")
    assert c.ci_low < 0.4 < c.ci_high
    assert c.correlation == pytest.approx(0.4, abs=0.06)
    assert c.asset_vol == pytest.approx(0.2, rel=0.08)
    assert c.fx_vol == pytest.approx(0.08, rel=0.08)


def test_asynchronous_closes_bias_daily_correlation_and_weekly_undoes_it():
    """The asset is observed half a day after the FX fixing: half of each
    day's common shock lands in the next day's asset return. Daily returns
    then see about half the correlation; weekly returns see nearly all of it."""
    rng = np.random.default_rng(7)
    n = len(DAYS)
    common = rng.standard_normal(n)
    own_a = rng.standard_normal(n)
    rho, dt = 0.6, 1 / 252
    # asset: half of today's common shock today, half tomorrow
    lagged = 0.5 * common + 0.5 * np.roll(common, 1)
    shock_a = rho * lagged + math.sqrt(1 - rho**2) * own_a
    shock_b = common
    a = _walk(0.2 * math.sqrt(dt) * shock_a, 100.0)
    b = _walk(0.08 * math.sqrt(dt) * shock_b, 1.1)
    daily = fx.correlation(a, b, "daily").correlation
    weekly = fx.correlation(a, b, "weekly").correlation
    assert daily < 0.45 < weekly


def test_realised_vol_recovers_the_simulated_volatility():
    rng = np.random.default_rng(3)
    s = _walk(0.1 * math.sqrt(1 / 252) * rng.standard_normal(len(DAYS)), 1.0)
    assert fx.realised_vol(s) == pytest.approx(0.1, rel=0.06)


def test_too_few_common_returns_is_refused_not_guessed():
    a, b = _correlated(0.3)
    with pytest.raises(fx.FxUnavailable, match="too few"):
        fx.correlation(a.iloc[:10], b.iloc[:10], "daily")


def _curve(ccy: str, z: float) -> rates.DiscountCurveSnapshot:
    times = [0.25, 0.5, 1, 2, 5, 10, 30]
    return rates.DiscountCurveSnapshot(
        ccy, date(2026, 9, 23), times, [math.exp(-z * t) for t in times]
    )


def test_forwards_are_covered_interest_parity(monkeypatch):
    curves = {"EUR": _curve("EUR", 0.03), "USD": _curve("USD", 0.045)}
    monkeypatch.setattr(rates, "currency_discount_curve", lambda c: curves[c])
    out = fx.forwards("EUR", "USD", 1.1367, date(2026, 9, 24))
    one_year = next(p for p in out.points if p.label == "1Y")
    # F = S · DF_EUR / DF_USD = S · exp((r_USD − r_EUR) T): EUR at a premium.
    assert one_year.forward == pytest.approx(1.1367 * math.exp(0.015), rel=1e-12)
    assert one_year.points > 0
    # nothing before the first pillar (3M): the 1M forward is not invented
    assert [p.label for p in out.points][0] == "3M"


def test_a_currency_without_a_curve_has_no_forward(monkeypatch):
    def curve(c):
        raise rates.RatesUnavailable("No free CHF government curve")

    monkeypatch.setattr(rates, "currency_discount_curve", curve)
    with pytest.raises(fx.FxUnavailable, match="CHF"):
        fx.forwards("EUR", "CHF", 0.94, date(2026, 9, 24))


# ── The endpoints ────────────────────────────────────────────────────────────


@pytest.fixture
def client(monkeypatch):
    a, b = _correlated(0.3)
    eur_usd = b

    def ecb(currencies, since):
        frame = pd.DataFrame({"USD": eur_usd.values, "GBP": 0.86}, index=eur_usd.index)
        frame["EUR"] = 1.0
        frame = frame[[c for c in currencies]]
        return frame[frame.index >= since] if since else frame

    monkeypatch.setattr(db, "ecb_fx_history", ecb)
    monkeypatch.setattr(
        db,
        "price_history",
        lambda t, since: [(d, v) for d, v in a.items() if d >= since],
    )
    curves = {"EUR": _curve("EUR", 0.03), "USD": _curve("USD", 0.045)}

    def curve(c):
        if c not in curves:
            raise rates.RatesUnavailable(f"No {c} curve.")
        return curves[c]

    monkeypatch.setattr(rates, "currency_discount_curve", curve)
    for name in ("_OVERVIEW_CACHE", "_HISTORY_CACHE", "_CORR_CACHE"):
        cache = getattr(market_fx, name)
        monkeypatch.setattr(market_fx, name, type(cache)(max_size=50, ttl_seconds=60))
    return TestClient(app)


def test_overview_has_spot_forwards_vol_and_methodology(client):
    d = client.get("/market/fx/overview?base=EUR&quote=USD").json()
    assert d["spot"] > 0 and d["forwards"] and d["forwards_unavailable"] is None
    assert set(d["realised_vol"]) == {"1Y", "3Y", "5Y"}
    assert "Forwards" in [s["title"] for s in d["methodology"]]


def test_overview_without_a_curve_says_why(client):
    d = client.get("/market/fx/overview?base=EUR&quote=GBP").json()
    assert d["forwards"] is None and "GBP" in d["forwards_unavailable"]


def test_correlation_endpoint_reports_its_uncertainty(client):
    d = client.get(
        "/market/fx/correlation?ticker=%5EFCHI&base=EUR&quote=USD&window=5Y"
    ).json()
    assert d["frequency"] == "weekly"
    assert d["ci_low"] < d["correlation"] < d["ci_high"] and d["n"] > 200


def test_same_currency_is_refused(client):
    assert client.get("/market/fx/overview?base=EUR&quote=EUR").status_code == 404
