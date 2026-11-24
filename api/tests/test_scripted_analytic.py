"""The closed form of a library product (engine='analytic', issue 313): the
European and digital calls and puts and the forward under Black-Scholes at
the stored smile's at-the-money implied vol. The products start today: the
strike is k times today's spot.

Properties: the closed form is the script's own Monte-Carlo price under the
same flat vol, within its error; its greeks are the formula's derivatives
at a fixed strike (checked against central differences of the closed form
itself, the strike held in money); call-put parity, and a digital call plus
its put pays the payout; a product without a closed form is refused.
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
    # A digital reads lastStepDigital (#363): its one step under flat vol is
    # the whole path, so every path pays the closed form, with no error.
    assert abs(exact.npv - mc.npv) <= max(4 * mc.mc_std_error, 1e-10 * abs(exact.npv))
    assert exact.diagnostics.startswith("Analytic ")
    assert exact.script == mc.script


@pytest.mark.parametrize("product", ps.ANALYTIC_SCRIPTED)
def test_the_greeks_are_the_formulas_derivatives(chain, product, monkeypatch):
    base = ps.price_scripted_product(_req(product, engine="analytic"))
    vol = ps._atm_implied_vol(
        SimpleNamespace(script=base.script, day_count="ACT/365F"), chain, SNAP, True
    )
    S0 = chain.spot

    def npv(spot=S0, sigma=vol, rate=RATE):
        # the strike held in money: k' S' = k S0
        m = SimpleNamespace(**{**chain.__dict__, "spot": spot})
        monkeypatch.setattr(market_snapshot, "local_vol_market", lambda *a: m)
        monkeypatch.setattr(ps, "_atm_implied_vol", lambda *a: sigma)
        return ps.price_scripted_product(
            _req(product, engine="analytic", rate=rate, terms={"strike": S0 / spot})
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


def test_call_put_parity_and_the_digitals_add_up(chain):
    def price(product, **terms):
        return ps.price_scripted_product(_req(product, engine="analytic", terms=terms))

    k = 1.05
    call, put = price("european-call", strike=k), price("european-put", strike=k)
    fwd = price("equity-forward", strike=k, notional=1)
    # C - P = S e^(-qT) - K e^(-rT), the forward struck at K
    assert call.npv - put.npv == pytest.approx(fwd.npv, rel=1e-10)
    assert call.greeks.delta - put.greeks.delta == pytest.approx(
        fwd.greeks.delta, rel=1e-10
    )
    assert (
        call.greeks.gamma == pytest.approx(put.greeks.gamma, rel=1e-12)
        and call.greeks.gamma > 0
    )
    dc, dp = price("digital-call", strike=k, payout=100), price(
        "digital-put", strike=k, payout=100
    )
    T = float(dc.diagnostics.split("T = ")[1].split(" ")[0])
    assert dc.npv + dp.npv == pytest.approx(100 * math.exp(-RATE * T), rel=1e-3)
    assert math.isfinite(dc.greeks.theta) and math.isfinite(call.greeks.theta)
