"""Swaption vols from traded premiums — `swaption_market.py`, `market_request`
and `/api/rates/market`, on the real C++ library and a fake store.

Trades are generated from a known normal vol with Bachelier's formula on a
known swap curve: the grid must give that vol back, platform straddles must
count for half their premium, and whatever cannot be read as an at-the-money
price must be left out and counted.
"""

from __future__ import annotations

import math
import os
from datetime import date, timedelta
from typing import List

os.environ.setdefault("JWT_SECRET", "test-only-secret-not-for-deployment")

import pytest
from fastapi.testclient import TestClient

import quantmodeling as qm
from api.app import db, rates_derivatives, swaption_market
from api.app.db import SwapRate, SwaptionTrade
from api.app.main import app

client = TestClient(app)

DAY = date(2026, 9, 30)
#: An upward-sloping SOFR par curve, 1Y to 30Y.
PAR = {
    1: 0.0455,
    2: 0.0473,
    3: 0.0477,
    5: 0.0478,
    7: 0.0481,
    10: 0.0486,
    15: 0.0500,
    20: 0.0503,
    30: 0.0494,
}
CURVE = [SwapRate(t, r, 50) for t, r in PAR.items()]
VOL = 0.0105  # 105 bp


def _forward(expiry: float, tenor: int):
    c = qm.bootstrap_ois_curve([], [(float(t), r) for t, r in PAR.items()])
    s = qm.price_swap(
        c["times"],
        c["discount_factors"],
        c["times"],
        c["discount_factors"],
        expiry,
        float(tenor),
        0.0,
        1,
        1,
        1.0,
        True,
        0.0,
    )
    return s["par_rate"], s["annuity"]


def _bachelier(
    payer: bool,
    forward: float,
    strike: float,
    expiry: float,
    vol: float,
    annuity: float,
) -> float:
    w = 1.0 if payer else -1.0
    sd = vol * math.sqrt(expiry)
    d = (forward - strike) / sd
    cdf = 0.5 * math.erfc(-w * d / math.sqrt(2.0))
    pdf = math.exp(-0.5 * d * d) / math.sqrt(2.0 * math.pi)
    return annuity * (w * (forward - strike) * cdf + sd * pdf)


def trade(
    expiry_years: float,
    tenor: int,
    *,
    vol: float = VOL,
    offset: float = 0.0,
    payer: bool = True,
    notional: float = 50e6,
    platform: str = "BILT",
    premium_factor: float = 1.0,
    strike_in_percent: bool = False,
    day: date = DAY,
) -> SwaptionTrade:
    """A swaption traded on `day` at `vol`, struck `offset` above the forward."""
    expiry = day + timedelta(days=round(expiry_years * 365.25))
    maturity = expiry + timedelta(days=round(tenor * 365.25))
    exact = (expiry - day).days / 365.25
    forward, annuity = _forward(exact, tenor)
    strike = forward + offset
    premium = (
        notional
        * _bachelier(payer, forward, strike, exact, vol, annuity)
        * premium_factor
    )
    return SwaptionTrade(
        day,
        day,
        "call" if payer else "put",
        expiry,
        maturity,
        strike * 100.0 if strike_in_percent else strike,
        notional,
        premium,
        platform,
    )


def grid(trades: List[SwaptionTrade], curves=None):
    return swaption_market.atm_normal_vols(
        trades, {DAY: CURVE} if curves is None else curves
    )


# ── the grid ─────────────────────────────────────────────────────────────────


def test_the_vol_that_priced_the_trades_comes_back():
    trades = [trade(1.0, 10, notional=n) for n in (25e6, 50e6, 100e6, 250e6)]
    trades += [trade(0.5, 5, vol=0.0120, payer=False) for _ in range(3)]
    g = grid(trades)
    assert [(p.expiry, p.tenor, p.trades) for p in g.points] == [
        (0.5, 5, 3),
        (1.0, 10, 4),
    ]
    assert g.points[0].normal_vol == pytest.approx(0.0120, rel=1e-6)
    assert g.points[1].normal_vol == pytest.approx(VOL, rel=1e-6)
    assert g.points[1].low == pytest.approx(VOL, rel=1e-6) and g.points[
        1
    ].high == pytest.approx(VOL, rel=1e-6)
    assert g.trades_used == 7 and g.rejected == {}


def test_the_point_is_the_median_and_resists_a_bad_print():
    trades = [trade(1.0, 10, vol=v) for v in (0.0100, 0.0104, 0.0105, 0.0106, 0.0210)]
    (point,) = grid(trades).points
    assert point.normal_vol == pytest.approx(
        0.0105, rel=1e-6
    )  # not pulled up by 210 bp
    assert point.low == pytest.approx(0.0104, rel=1e-6) and point.high == pytest.approx(
        0.0106, rel=1e-6
    )
    assert point.trades == 5


def test_a_platform_straddle_counts_for_half_its_premium():
    # On a platform each leg of the straddle is reported with the premium of
    # both legs: at the money, twice its own.
    def straddle(platform: str, factor: float):
        legs = []
        for notional in (50e6, 75e6, 100e6):
            legs.append(
                trade(
                    1.0,
                    10,
                    payer=True,
                    notional=notional,
                    platform=platform,
                    premium_factor=factor,
                )
            )
            legs.append(
                trade(
                    1.0,
                    10,
                    payer=False,
                    notional=notional,
                    platform=platform,
                    premium_factor=factor,
                )
            )
        return legs

    (on_platform,) = grid(straddle("ISWV", 2.0)).points
    assert (
        on_platform.normal_vol == pytest.approx(VOL, rel=1e-6)
        and on_platform.trades == 6
    )
    # Bilateral: each leg carries its own premium and is read as is.
    (bilateral,) = grid(straddle("BILT", 1.0)).points
    assert bilateral.normal_vol == pytest.approx(VOL, rel=1e-6)
    # A single leg on a platform is not a straddle: nothing is halved.
    (single,) = grid(
        [trade(1.0, 10, platform="ISWV", notional=n) for n in (50e6, 75e6, 100e6)]
    ).points
    assert single.normal_vol == pytest.approx(VOL, rel=1e-6)


def test_trades_that_are_not_an_at_the_money_price_are_counted_not_used():
    atm = [trade(1.0, 10, notional=n) for n in (25e6, 50e6, 100e6)]
    g = grid(
        atm
        + [trade(1.0, 10, offset=0.0100)]  # 100 bp out of the money
        + [trade(1.0, 10, offset=-0.0100)]  # in the money: payer or receiver matters
        + [trade(5.0, 10)]  # premium convention unknown beyond two years
        + [trade(1.0, 4)]  # no four-year bucket
        + [trade(1.0, 10, premium_factor=40.0)]  # a premium in the wrong unit
    )
    assert [p.trades for p in g.points] == [3]
    assert g.rejected == {
        "away from the money": 2,
        "off the grid (expiry beyond two years, odd tenor)": 2,
        "implausible premium": 1,
    }


def test_small_moneyness_is_tolerated_and_a_percent_strike_understood():
    # 1 bp from the forward the two readings of "call" agree within tolerance.
    near = [trade(1.0, 10, offset=o) for o in (-0.0001, 0.0, 0.0001)]
    (point,) = grid(near).points
    assert point.normal_vol == pytest.approx(VOL, rel=0.03)
    percent = [
        trade(1.0, 10, strike_in_percent=True, notional=n) for n in (25e6, 50e6, 100e6)
    ]
    assert grid(percent).points[0].normal_vol == pytest.approx(VOL, rel=1e-6)


def test_too_few_trades_or_no_curve_give_no_point():
    g = grid([trade(1.0, 10), trade(1.0, 10)])
    assert g.points == [] and g.rejected == {"bucket with too few trades": 2}
    g = grid([trade(1.0, 10)] * 3, curves={})
    assert g.points == [] and g.rejected == {"no swap curve that day": 3}
    # A curve that stops before the swap's maturity cannot price it.
    short = {DAY: [r for r in CURVE if r.tenor_years <= 7]}
    assert grid([trade(1.0, 10)] * 3, curves=short).rejected == {
        "no swap curve that day": 3
    }


# ── market_request and the endpoint ──────────────────────────────────────────


@pytest.fixture
def store(monkeypatch):
    """A store holding five days of SOFR curves and swaption trades."""
    days = [DAY - timedelta(days=k) for k in (0, 1, 2, 5, 6)]
    state = {"curves": {d: CURVE for d in days}, "trades": []}
    for d in days:
        for expiry, tenor in ((0.5, 10), (1.0, 2), (1.0, 5), (1.0, 10), (2.0, 10)):
            # Vol falling with the tenor, as a mean-reverting model wants.
            state["trades"].append(
                trade(expiry, tenor, vol=0.0125 - 0.0002 * tenor, day=d)
            )
    monkeypatch.setattr(
        db,
        "dtcc_swap_curves",
        lambda product, since: {d: c for d, c in state["curves"].items() if d >= since},
    )
    monkeypatch.setattr(
        db,
        "dtcc_swaption_trades",
        lambda underlier, since: [t for t in state["trades"] if t.report_date >= since],
    )
    monkeypatch.setattr(
        rates_derivatives,
        "date",
        type(
            "FixedDate",
            (date,),
            {"today": classmethod(lambda cls: DAY + timedelta(days=1))},
        ),
    )
    return state


def test_market_request_carries_the_quotes_and_what_they_rest_on(store):
    m = rates_derivatives.market_request()
    assert m["currency"] == "USD" and m["as_of"] == DAY
    assert m["window_start"] == DAY - timedelta(
        days=rates_derivatives.MARKET_WINDOW_DAYS
    )
    assert [q["tenor"] for q in m["ois"]] == [float(t) for t in PAR]
    assert m["swaps"] == m["ois"] and m["deposits"] == [] and m["fras"] == []
    assert len(m["swaption_vols"]) == 5 and all(
        v["trades"] == 5 for v in m["market_vols"]
    )
    assert m["trades_used"] == 25 and m["rejected"] == []
    assert "not dealer quotes" in m["label"] and DAY.isoformat() in m["label"]


def test_the_endpoint_feeds_the_analysis_and_hull_white_fits_the_traded_vols(store):
    r = client.get("/api/rates/market")
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["as_of"] == DAY.isoformat() and body["trades_used"] == 25
    assert {(v["expiry"], v["tenor"]) for v in body["swaption_vols"]} == {
        (0.5, 10.0),
        (1.0, 2.0),
        (1.0, 5.0),
        (1.0, 10.0),
        (2.0, 10.0),
    }

    analysis = client.post("/api/rates/analyse", json=body["request"])
    assert analysis.status_code == 200, analysis.text
    result = analysis.json()
    # One curve discounts and projects: the swaps reprice and there is no basis.
    assert result["curves"]["max_repricing_error_bp"] < 1e-6
    assert all(abs(p["basis_bp"]) < 1e-6 for p in result["curves"]["points"])
    hw = result["hull_white"]
    assert hw["converged"] and hw["rmse_bp"] < 3.0
    assert 0.008 < hw["sigma"] < 0.016 and hw["mean_reversion"] > 0.0


def test_no_fallback_when_the_store_has_nothing_usable(store):
    # Stale: the latest curve is older than the limit.
    stale = DAY - timedelta(days=rates_derivatives.MARKET_STALE_AFTER_DAYS + 3)
    store["curves"] = {stale: CURVE}
    with pytest.raises(rates_derivatives.RatesMarketUnavailable, match="days ago"):
        rates_derivatives.market_request()
    r = client.get("/api/rates/market")
    assert r.status_code == 503 and stale.isoformat() in r.json()["message"]

    # A curve but no swaption trade.
    store["curves"] = {DAY: CURVE}
    store["trades"] = []
    with pytest.raises(
        rates_derivatives.RatesMarketUnavailable, match="swaption vol points"
    ):
        rates_derivatives.market_request()

    # No curve at all.
    store["curves"] = {}
    with pytest.raises(
        rates_derivatives.RatesMarketUnavailable, match="No SOFR swap curve"
    ):
        rates_derivatives.market_request()


def test_an_unreachable_store_is_a_503(monkeypatch):
    def down(*_):
        raise db.StoreUnavailable("connection refused")

    monkeypatch.setattr(db, "dtcc_swap_curves", down)
    r = client.get("/api/rates/market")
    assert (
        r.status_code == 503 and r.json()["message"] == "Market data store unavailable"
    )
