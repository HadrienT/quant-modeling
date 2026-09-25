"""Vega by quoted option (market_vega.py, the Dupire superbucket of lot 17h),
on a synthetic option chain run through the real pipeline: clean -> SVI ->
Dupire -> local-vol AAD -> superbucket. The property checked is the one a
desk would: the quote vegas add up to what moving the whole chain by one vol
point does to the price, recalibrated and repriced."""

import math
from datetime import date

import pytest
import quantmodeling as qm

from app import market_snapshot, market_vega
from app.schemas import ScriptedProductRequest

SNAP = date(2026, 9, 11)
SPOT, RATE, DIV = 100.0, 0.03, 0.0
EXPIRIES = (0.25, 0.5, 1.0, 1.5)


def _iv(k: float, T: float) -> float:
    """A skewed smile: SVI total variance, put skew."""
    x = k - 0.02
    w = 0.03 * T + 0.12 * math.sqrt(T) * (-0.6 * x + math.sqrt(x * x + 0.15**2))
    return math.sqrt(w / T)


def _chain(shift: float = 0.0):
    n = lambda z: 0.5 * math.erfc(-z / math.sqrt(2))  # noqa: E731
    quotes = []
    for T in EXPIRIES:
        F = SPOT * math.exp((RATE - DIV) * T)
        for K in range(75, 136, 5):
            iv = _iv(math.log(K / F), T) + shift
            d1 = (math.log(F / K) + 0.5 * iv * iv * T) / (iv * math.sqrt(T))
            call = math.exp(-RATE * T) * (F * n(d1) - K * n(d1 - iv * math.sqrt(T)))
            q = qm.RawOptionQuote()
            q.strike, q.ttm, q.is_call = float(K), T, True
            q.bid, q.ask, q.last = call * 0.995, call * 1.005, call
            q.volume, q.open_interest = 100, 1000
            q.implied_vol, q.has_iv = iv, True
            quotes.append(q)
    return quotes


def _market(shift: float = 0.0) -> market_snapshot.LocalVolMarket:
    ms = market_snapshot
    r = qm.calibrate_vol_surface(
        _chain(shift),
        SPOT,
        RATE,
        DIV,
        ms.K_MIN,
        ms.K_MAX,
        ms.N_STRIKES,
        ms.N_MATURITIES,
        cleaning_params=qm.CleaningParams(),
    )
    return ms.LocalVolMarket(
        ticker="TEST",
        valuation_date=SNAP,
        spot=SPOT,
        dividend=DIV,
        K_grid=r["K_grid"],
        T_grid=r["T_grid"],
        sigma_loc_flat=r["sigma_loc_flat"],
        svi_slices=[dict(s) for s in r["slices"]],
        rate=RATE,
        k_min=r["k_min"],
        k_max=r["k_max"],
    )


@pytest.fixture
def chain(monkeypatch):
    m = _market()
    monkeypatch.setattr(market_snapshot, "local_vol_market", lambda *a: m)
    return m


def _req(**kw):
    base = dict(
        product="european-call",
        ticker="TEST",
        rate=RATE,
        valuation_date=SNAP,
        n_paths=16_000,
        seed=5,
    )
    return ScriptedProductRequest(**{**base, **kw})


def test_every_quote_gets_a_vega_with_its_error(chain):
    out = market_vega.market_vega(_req())
    n_quotes = sum(len(s["quotes"]) for s in chain.svi_slices)
    assert len(out.quotes) == n_quotes > 30
    assert all(q.std_error >= 0 for q in out.quotes)
    assert len(out.maturities) == len(EXPIRIES)
    assert out.total_vega > 0  # a call is long vol


def test_the_quote_vegas_add_up_to_moving_the_whole_chain(chain, monkeypatch):
    out = market_vega.market_vega(_req())
    h = 0.005  # half a vol point each way

    def price(shift):
        m = _market(shift)
        total = 0.0
        for b in range(market_vega.BATCHES):
            total += qm.price_script(
                __import__("app").product_templates.render("european-call", {}, SNAP),
                SPOT,
                RATE,
                DIV,
                0.0,
                SNAP.isoformat(),
                n_paths=16_000 // market_vega.BATCHES,
                seed=5 + b,
                model="local_vol",
                K_grid=m.K_grid,
                T_grid=m.T_grid,
                sigma_loc_flat=m.sigma_loc_flat,
                steps_per_year=52,
            )["npv"]
        return total / market_vega.BATCHES

    bumped = (price(h) - price(-h)) / (2 * h) * 0.01  # per vol point
    assert out.total_vega == pytest.approx(bumped, rel=0.1, abs=4 * out.total_std_error)


def test_the_vega_sits_at_the_product_maturity(chain):
    # A one-year European call: the maturities that bracket one year carry
    # most of the vega.
    out = market_vega.market_vega(_req())
    near = sum(m.vega for m in out.maturities if 0.5 <= m.ttm <= 1.5)
    assert near > 0.7 * out.total_vega


def test_several_underlyings_are_refused():
    with pytest.raises(ValueError, match="one underlying"):
        market_vega.market_vega(
            _req(
                product="worst-of-call",
                ticker=None,
                underlyings=[{"ticker": "A"}, {"ticker": "B"}, {"ticker": "C"}],
            )
        )
