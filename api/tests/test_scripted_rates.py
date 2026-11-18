"""Rates products of the library (`# market: rates`, issue 313 lot b2a):
bonds priced on the currency's government curve, by Hull-White fitted to it
(Monte-Carlo) or by the curve itself (engine='analytic': the script valued
with no short-rate volatility).

Properties: the analytic price is the sum of the flows' discount factors on
the curve; Monte-Carlo agrees within its error; a ticker or another model is
refused; the request asks for no stock input.
"""

import math
from datetime import date

import pytest

from app import pricing_service as ps
from app import rates
from app.schemas import ScriptedProductRequest

SNAP = date(2026, 11, 17)
TIMES = [0.25, 0.5, 1.0, 2.0, 3.0, 5.0, 7.0, 10.0]
ZEROS = [0.040, 0.041, 0.042, 0.041, 0.040, 0.041, 0.042, 0.043]
DFS = [math.exp(-z * t) for z, t in zip(ZEROS, TIMES)]


@pytest.fixture
def curve(monkeypatch):
    snap = rates.DiscountCurveSnapshot("USD", SNAP, TIMES, DFS)
    monkeypatch.setattr(rates, "currency_discount_curve", lambda ccy: snap)
    return snap


def _req(product, **kw):
    return ScriptedProductRequest(product=product, valuation_date=SNAP, **kw)


def _P(t):
    # log-linear in the discount factors between pillars (the C++ curve)
    j = 1
    while j + 1 < len(TIMES) and TIMES[j] < t:
        j += 1
    w = (t - TIMES[j - 1]) / (TIMES[j] - TIMES[j - 1])
    return math.exp((1 - w) * math.log(DFS[j - 1]) + w * math.log(DFS[j]))


def test_the_zero_coupon_is_its_discount_factor(curve):
    import quantmodeling as qm

    r = ps.price_scripted_product(_req("zero-coupon-bond", engine="analytic"))
    events = qm.validate_script(r.script, SNAP.isoformat())["events"]
    assert r.npv == pytest.approx(100.0 * _P(events[-1]["t"]), rel=1e-6)
    assert r.mc_std_error == 0.0
    assert "government curve" in r.diagnostics


@pytest.mark.parametrize("product", ["zero-coupon-bond", "fixed-rate-bond"])
def test_monte_carlo_agrees_with_the_curve(curve, product):
    exact = ps.price_scripted_product(_req(product, engine="analytic"))
    mc = ps.price_scripted_product(_req(product, n_paths=50_000, seed=3))
    assert mc.model_choice.model == "hull_white"
    assert abs(mc.npv - exact.npv) < 4 * mc.mc_std_error + 1e-6


def test_a_higher_coupon_is_worth_more(curve):
    low = ps.price_scripted_product(
        _req("fixed-rate-bond", engine="analytic", terms={"coupon": 0.03})
    )
    high = ps.price_scripted_product(
        _req("fixed-rate-bond", engine="analytic", terms={"coupon": 0.05})
    )
    assert low.npv < high.npv


def test_a_rates_product_takes_no_ticker_nor_another_model():
    with pytest.raises(ValueError, match="rates product"):
        _req("zero-coupon-bond", ticker="AAPL")
    with pytest.raises(ValueError, match="rates product"):
        _req("zero-coupon-bond", model="local_vol")
    req = _req("zero-coupon-bond")  # no spot, vol nor rate asked for
    assert req.model == "hull_white"


def test_a_zero_coupons_adjoint_risk_is_its_duration(curve):
    import quantmodeling as qm

    r = ps.price_scripted_product(
        _req("zero-coupon-bond", n_paths=20_000, seed=2, greeks_method="aad")
    )
    T = qm.validate_script(r.script, SNAP.isoformat())["events"][-1]["t"]
    by = {x.label: x.value for x in r.risks}
    # every path pays P(0, T) e^{-I}: d/dz = -T times it, path by path
    assert by["curve_parallel"] == pytest.approx(-T * r.npv, rel=1e-9)
    pillars = [v for k, v in by.items() if k.startswith("zero[")]
    assert len(pillars) == len(TIMES)
    assert sum(pillars) == pytest.approx(by["curve_parallel"], rel=1e-9)
    assert r.greeks.rho == by["curve_parallel"]
