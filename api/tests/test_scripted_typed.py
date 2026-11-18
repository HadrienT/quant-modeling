"""Library products priced on inputs typed by hand -- no ticker, no curve
from the database: a fictitious underlying or market, chosen entirely by the
user.

Properties, every database read made to fail: the European's closed form is
Black-Scholes on the typed spot, vol, rate and dividend, and its Monte-Carlo
agrees; a basket prices on typed spots, vols and a correlation; a
zero-coupon on a flat typed curve is its discount factor, a swap at its
forward rate is worth nothing, and a swaption's Monte-Carlo agrees with its
closed form on that curve.
"""

import math
from datetime import date

import pytest

from app import market_snapshot, multi_asset_market, rates, rates_derivatives
from app import pricing_service as ps
from app import product_templates as pt
from app.schemas import ScriptedProductRequest

V = date(2026, 6, 1)


@pytest.fixture(autouse=True)
def no_database(monkeypatch):
    def refuse(*a, **k):
        raise AssertionError("a typed pricing read the database")

    monkeypatch.setattr(market_snapshot, "local_vol_market", refuse)
    monkeypatch.setattr(multi_asset_market, "multi_asset_market", refuse)
    monkeypatch.setattr(rates, "currency_discount_curve", refuse)
    monkeypatch.setattr(rates_derivatives, "sofr_hull_white", refuse)


def _bs_call(S, K, r, q, v, T):
    sd = v * math.sqrt(T)
    d1 = (math.log(S / K) + (r - q + 0.5 * v * v) * T) / sd
    n = lambda x: 0.5 * math.erfc(-x / math.sqrt(2))  # noqa: E731
    return S * math.exp(-q * T) * n(d1) - K * math.exp(-r * T) * n(d1 - sd)


def _typed(product, **kw):
    base = dict(
        product=product,
        valuation_date=V,
        spot=80.0,
        vol=0.3,
        dividend=0.02,
        rate=0.05,
        n_paths=100_000,
        seed=4,
    )
    return ScriptedProductRequest(**{**base, **kw})


def test_the_typed_european_is_black_scholes_on_its_inputs():
    exact = ps.price_scripted_product(_typed("european-call", engine="analytic"))
    T = float(exact.diagnostics.split("T = ")[1].split(" ")[0])
    assert exact.npv == pytest.approx(_bs_call(80, 80, 0.05, 0.02, 0.3, T), rel=1e-12)
    assert "typed" in exact.diagnostics
    mc = ps.price_scripted_product(_typed("european-call", model="black_scholes"))
    assert abs(mc.npv - exact.npv) < 4 * mc.mc_std_error


def test_a_basket_prices_on_typed_underlyings():
    r = ps.price_scripted_product(
        ScriptedProductRequest(
            product="basket-call",
            valuation_date=V,
            underlyings=[
                {"spot": 50.0, "vol": 0.2},
                {"spot": 120.0, "vol": 0.35, "dividend": 0.01},
                {"spot": 10.0, "vol": 0.5},
            ],
            correlation=[[1, 0.3, 0.3], [0.3, 1, 0.3], [0.3, 0.3, 1]],
            rate=0.03,
            n_paths=20_000,
        )
    )
    assert r.npv > 0
    assert [u.vol_source for u in r.model_choice.underlyings] == ["typed"] * 3


def _flat(product, **kw):
    hw = {"curve": "flat", "mean_reversion": 0.05, "sigma": 0.01}
    return ScriptedProductRequest(
        product=product, valuation_date=V, rate=0.04, hull_white=hw, **kw
    )


def test_a_zero_coupon_on_a_flat_curve_is_its_discount_factor():
    r = ps.price_scripted_product(_flat("zero-coupon-bond", engine="analytic"))
    script = pt.render("zero-coupon-bond", {}, V)
    import quantmodeling as qm

    T = qm.validate_script(script, V.isoformat())["events"][-1]["t"]
    assert r.npv == pytest.approx(100 * math.exp(-0.04 * T), rel=1e-9)
    assert "flat curve at 4.0000% (typed)" in r.diagnostics


def test_a_swap_at_its_forward_rate_is_worth_nothing_on_a_flat_curve():
    def value(k):
        return ps.price_scripted_product(
            _flat("interest-rate-swap", engine="analytic", terms={"fixed_rate": k})
        )

    # linear in the fixed rate: the forward rate is where it crosses zero
    v1, v2 = value(0.03).npv, value(0.05).npv
    par = 0.03 - v1 * (0.05 - 0.03) / (v2 - v1)
    r = value(par)
    assert abs(r.npv) < 1e-6 * 10_000_000
    assert par == pytest.approx(math.exp(0.04) - 1, abs=2e-4)  # ~ the flat rate, annual
    assert r.model_choice.rates_calibration is None


def test_a_swaption_monte_carlo_agrees_with_its_closed_form_on_a_flat_curve():
    exact = ps.price_scripted_product(
        _flat("european-swaption", engine="analytic", maturity=3, expiry=2)
    )
    mc = ps.price_scripted_product(
        _flat("european-swaption", maturity=3, expiry=2, n_paths=100_000, seed=2)
    )
    assert exact.npv > 0
    assert abs(mc.npv - exact.npv) < 4 * mc.mc_std_error
    # the asked lengths reach the closed form (2y into 3y)
    assert exact.script.count("df(") > 3
