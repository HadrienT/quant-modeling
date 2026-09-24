"""The desk-model policy (portfolio_models.py) on the fake market of
test_portfolio_valuation, with a synthetic smile where one is wanted.

Properties, not numbers: the SVI surface returns each slice's own vol at its
maturity; a digital marked on the smile is the strike derivative of the
calls priced on that smile; a local-vol Monte-Carlo on a flat surface is
Black-Scholes; a knocked-out barrier is worth nothing and a knocked-in one
is the vanilla; an Asian carries the average already fixed; every mark says
which model priced it."""

from __future__ import annotations

import math
from datetime import date

import pytest

from api.app import portfolio_models as models
from api.app import portfolio_valuation as pv
from api.app import valuation, vol_smile
from api.app.portfolio_schemas import (
    DerivativeSpec,
    Instrument,
    Portfolio,
    Trade,
)
from api.tests.test_portfolio_valuation import CLOSES, END, market  # noqa: F401

TTMS = (0.25, 0.5, 1.0, 2.0)


def _slice(T: float) -> dict:
    # A skewed smile: ~20 % ATM, higher vol for low strikes.
    return {
        "ttm": T,
        "a": 0.03 * T,
        "b": 0.08 * T,
        "rho": -0.6,
        "m": 0.0,
        "sigma": 0.2,
        "rmse": 0.001,
        "converged": True,
    }


def _smile(flat_lv: float = 0.0) -> vol_smile.Smile:
    spot = CLOSES["AAA"][END]
    K = tuple(spot * x for x in (0.3, 0.6, 0.8, 1.0, 1.2, 1.5, 2.5))
    T = (0.05, 0.5, 1.0, 3.0)
    lv = flat_lv or 0.2
    return vol_smile.Smile(
        ticker="AAA",
        snapshot=END,
        spot=spot,
        rate=0.02,
        dividend=0.0,
        slices=tuple(_slice(t) for t in TTMS),
        K_grid=K,
        T_grid=T,
        sigma_loc_flat=tuple(lv for _ in K for _ in T),
    )


@pytest.fixture
def smile(monkeypatch):
    s = _smile()
    monkeypatch.setattr(
        vol_smile, "smile_for", lambda t, d, rate_at: s if t == "AAA" else None
    )
    return s


def _spec(product: str, expiry=date(2027, 3, 19), **params) -> DerivativeSpec:
    base = {
        "spot": 1.0,
        "strike": 105.0,
        "maturity": 1.0,
        "rate": 0.02,
        "dividend": 0.0,
        "vol": 0.9,
        "is_call": True,
    }
    return DerivativeSpec(
        product=product,
        underlying="AAA",
        currency="EUR",
        expiry=expiry,
        params={**base, **params},
    )


def _mark(spec: DerivativeSpec, d: date = END):
    inst = Instrument(id="x", spec=spec)
    return pv.mark(inst, d, pv.MarketData(date(2026, 1, 5)), today=END)


# ── The smile ────────────────────────────────────────────────────────────────


def test_the_svi_surface_returns_each_slice_at_its_own_maturity():
    s = _smile()
    for sl in s.slices:
        K = s.forward(sl["ttm"]) * math.exp(0.1)  # k = 0.1
        w = sl["a"] + sl["b"] * (sl["rho"] * 0.1 + math.sqrt(0.01 + sl["sigma"] ** 2))
        assert s.implied_vol(K, sl["ttm"]) == pytest.approx(math.sqrt(w / sl["ttm"]))


def test_beyond_the_slices_the_implied_vol_is_held_flat_in_maturity():
    s = _smile()
    K = s.spot
    assert s.implied_vol(K, 5.0) == pytest.approx(s.implied_vol(K, TTMS[-1]))
    assert s.implied_vol(K, 0.01) == pytest.approx(s.implied_vol(K, TTMS[0]))


# ── Vanillas and digitals ────────────────────────────────────────────────────


def test_a_vanilla_is_marked_at_the_smiles_implied_vol_for_its_strike(smile):
    m = _mark(_spec("vanilla", engine="analytic"))
    T = (date(2027, 3, 19) - END).days / 365.25
    assert m.model.model == "Black-Scholes on the SVI smile"
    [vol] = [p for p in m.model.params if p.name == "implied vol at K, T"]
    assert vol.status == "calibrated"
    assert vol.value == pytest.approx(smile.implied_vol(105.0, T))


def test_without_a_smile_a_vanilla_falls_back_to_the_realised_vol_proxy():
    m = _mark(_spec("vanilla", engine="analytic"))
    assert m.model.model == "Black-Scholes, flat volatility"
    assert any(p.status == "proxied" for p in m.model.params)


def test_a_digital_on_the_smile_is_minus_the_strike_derivative_of_the_calls(smile):
    """The call-spread replication, checked against a finite difference of
    calls each priced at its own strike's implied vol."""
    m = _mark(_spec("digital", payoff_type="cash-or-nothing", cash_amount=1.0))
    S = CLOSES["AAA"][END]
    T = (date(2027, 3, 19) - END).days / 365.25
    r = next(p.value for p in m.model.params if p.name == "rate")

    def call(K: float) -> float:
        req = valuation.PRODUCTS["vanilla"].request.model_validate(
            {
                "spot": S,
                "strike": K,
                "maturity": T,
                "rate": r,
                "dividend": 0.0,
                "vol": smile.implied_vol(K, T),
                "is_call": True,
                "engine": "analytic",
            }
        )
        return valuation.price("vanilla", req).response.npv

    h = 0.05
    replication = -(call(105.0 + h) - call(105.0 - h)) / (2 * h)
    assert m.value == pytest.approx(replication, rel=2e-3)
    # and the skew term is not negligible: a flat-vol digital misses it
    [corr] = [p for p in m.model.params if p.name == "skew correction"]
    assert abs(corr.value) > 1e-3


# ── Path-dependent products: scripts with past fixings ───────────────────────

START = date(2026, 6, 1)


def _scripted(product: str, **params):
    return _spec(
        product, expiry=date(2027, 3, 19), start_date=START.isoformat(), **params
    )


def test_local_vol_on_a_flat_surface_is_black_scholes(monkeypatch):
    """Dupire local vol with σ_loc ≡ 20 % is Black-Scholes at 20 %: the same
    script priced both ways agrees within Monte-Carlo error."""
    flat = _smile(flat_lv=0.2)
    spec = _scripted("asian", average_type="arithmetic")
    monkeypatch.setattr(vol_smile, "smile_for", lambda t, d, rate_at: flat)
    lv = _mark(spec)
    monkeypatch.setattr(vol_smile, "smile_for", lambda t, d, rate_at: None)
    monkeypatch.setattr(pv.MarketData, "realised_vol", lambda self, t, d: (d, 0.2))
    monkeypatch.setattr(pv, "_STORE", pv.MarkStore())
    bs = _mark(spec)
    assert lv.model.model == "Dupire local volatility"
    assert bs.model.model == "Black-Scholes, flat volatility"
    se = math.hypot(lv.model.std_error, bs.model.std_error)
    assert lv.value == pytest.approx(bs.value, abs=4 * se)


def test_an_asian_carries_the_average_already_fixed():
    m = _mark(_scripted("asian", average_type="arithmetic"))
    done = [c for d, c in sorted(CLOSES["AAA"].items()) if START <= d <= END]
    state = {p.name: p.value for p in m.model.params}
    assert state["average so far"] == pytest.approx(sum(done) / len(done))
    assert state["fixings done"] == len(done)


def test_a_knocked_out_barrier_is_worth_nothing_and_a_knocked_in_one_the_vanilla():
    level = min(c for d, c in CLOSES["AAA"].items() if START <= d <= END) * 1.001
    out = _mark(
        _scripted(
            "barrier", barrier_level=level, barrier_kind="down-and-out", rebate=0.0
        )
    )
    assert out.value == pytest.approx(0.0, abs=1e-12)
    assert {p.name: p.value for p in out.model.params}["barrier crossed"] == 1.0
    knocked_in = _mark(
        _scripted(
            "barrier", barrier_level=level, barrier_kind="down-and-in", rebate=0.0
        )
    )
    vanilla = _mark(_spec("vanilla", engine="analytic", expiry=date(2027, 3, 19)))
    se = knocked_in.model.std_error
    assert knocked_in.value == pytest.approx(
        vanilla.value, abs=4 * se + 0.02 * vanilla.value
    )


def test_a_lookback_is_worth_at_least_the_extremum_already_reached():
    m = _mark(
        _scripted("lookback", style="fixed-strike", extremum="maximum", strike=90.0)
    )
    hi = max(c for d, c in CLOSES["AAA"].items() if START <= d <= END)
    T = (date(2027, 3, 19) - END).days / 365.25
    r = next(p.value for p in m.model.params if p.name == "rate")
    assert m.value >= (hi - 90.0) * math.exp(-r * T) - 3 * m.model.std_error


def test_a_path_dependent_product_starts_at_its_first_trade_when_no_start_is_given():
    spec = _spec("asian", average_type="arithmetic")
    pf = Portfolio(
        id="p",
        version=2,
        instruments=[Instrument(id="a", spec=spec)],
        transactions=[
            Trade(
                id="1",
                instrument_id="a",
                trade_date=date(2026, 7, 6),
                quantity=1,
                price=5.0,
            )
        ],
    )
    [inst] = pv.with_contract_starts(pf).instruments
    assert inst.spec.params["start_date"] == "2026-07-06"


# ── Every derivative mark says which model priced it ─────────────────────────


def test_the_snapshot_carries_each_derivatives_model(smile):
    spec = _spec("vanilla", engine="analytic")
    pf = Portfolio(
        id="p",
        version=2,
        base_currency="EUR",
        instruments=[Instrument(id="c", spec=spec)],
        transactions=[
            Trade(
                id="1",
                instrument_id="c",
                trade_date=date(2026, 9, 1),
                quantity=2,
                price=5.0,
            )
        ],
    )
    [pos] = pv.snapshot(pf, END).positions
    assert pos.model is not None and pos.model.engine == "Black-Scholes closed form"
    assert pos.model.why


def test_a_change_of_method_never_serves_marks_stored_by_the_old_one(monkeypatch):
    inst = Instrument(id="x", spec=_spec("vanilla", engine="analytic"))
    before = pv.MarkStore.key(inst)
    monkeypatch.setattr(pv.MarkStore, "METHOD", pv.MarkStore.METHOD + 1)
    assert pv.MarkStore.key(inst) != before


def test_the_policy_covers_every_product_the_trade_form_offers():
    offered = {
        "vanilla",
        "american_vanilla",
        "quanto",
        "asian",
        "barrier",
        "digital",
        "lookback",
        "future",
    }
    assert offered <= set(models.POLICY) | models.SCRIPTED
