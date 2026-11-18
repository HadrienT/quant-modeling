"""The closed form of a library product (engine='analytic', issue 313): the
European call and the digital call under Black-Scholes at the stored
smile's at-the-money implied vol, the strike fixed on the start date.

Properties: the closed form is the script's own Monte-Carlo price under the
same flat vol, within its error; its greeks are the formula's derivatives
(checked against central differences of the closed form itself); a product
without a closed form is refused.
"""

import math
from types import SimpleNamespace

import pytest

from app import market_snapshot, pricing_service as ps
from app.schemas import ScriptedProductRequest
from test_market_vega import RATE, SNAP, _market


@pytest.fixture
def chain(monkeypatch):
    m = _market()
    monkeypatch.setattr(market_snapshot, "local_vol_market", lambda *a: m)
    return m


def _req(product: str, **kw) -> ScriptedProductRequest:
    base = dict(product=product, ticker="TEST", rate=RATE, valuation_date=SNAP)
    return ScriptedProductRequest(**{**base, **kw})


@pytest.mark.parametrize("product", ps.ANALYTIC_SCRIPTED)
def test_the_closed_form_is_the_scripts_flat_vol_price(chain, product):
    exact = ps.price_scripted_product(_req(product, engine="analytic"))
    mc = ps.price_scripted_product(
        _req(product, model="black_scholes", n_paths=200_000, seed=3)
    )
    assert exact.mc_std_error == 0.0
    assert abs(exact.npv - mc.npv) < 4 * mc.mc_std_error
    assert "Analytic forward-start" in exact.diagnostics
    assert exact.script == mc.script


@pytest.mark.parametrize("product", ps.ANALYTIC_SCRIPTED)
def test_the_greeks_are_the_formulas_derivatives(chain, product, monkeypatch):
    base = ps.price_scripted_product(_req(product, engine="analytic"))
    vol = ps._atm_implied_vol(
        SimpleNamespace(script=base.script, day_count="ACT/365F"), chain, SNAP
    )

    def npv(spot=chain.spot, sigma=vol, rate=RATE):
        m = SimpleNamespace(**{**chain.__dict__, "spot": spot})
        monkeypatch.setattr(market_snapshot, "local_vol_market", lambda *a: m)
        monkeypatch.setattr(ps, "_atm_implied_vol", lambda *a: sigma)
        return ps.price_scripted_product(
            _req(product, engine="analytic", rate=rate)
        ).npv

    g = base.greeks
    h = 1e-4
    assert g.delta == pytest.approx(
        (npv(spot=chain.spot * (1 + h)) - npv(spot=chain.spot * (1 - h)))
        / (2 * h * chain.spot),
        abs=1e-6,
    )
    assert g.vega == pytest.approx(
        (npv(sigma=vol + h) - npv(sigma=vol - h)) / (2 * h), rel=1e-5
    )
    assert g.rho == pytest.approx(
        (npv(rate=RATE + h) - npv(rate=RATE - h)) / (2 * h), rel=1e-5
    )


def test_a_product_without_a_closed_form_is_refused(chain):
    with pytest.raises(ValueError, match="no closed form"):
        ps.price_scripted_product(_req("asian-call", engine="analytic"))


def test_the_call_is_s0_times_the_unit_forward_start(chain):
    # S0 e^(-q t1) C(1, k, tau): homogeneous of degree one in the spot.
    r = ps.price_scripted_product(_req("european-call", engine="analytic"))
    assert r.greeks.delta == pytest.approx(r.npv / chain.spot, rel=1e-12)
    assert r.greeks.gamma == 0.0
    assert math.isfinite(r.greeks.theta)
