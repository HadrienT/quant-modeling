"""Script positions of a portfolio (portfolio_scripts.py, issue 313 lot b3),
on the fake market of test_portfolio_valuation (equities) and a flat
synthetic SOFR history (rates).

Properties: a booked script keeps the contract's dates; a position
converted from a hand-written pricer marks exactly as it did; the library's
European, once its strike is fixed, is the vanilla of that strike; flows
already paid stay in the mark, so a swap's mark does not jump on a reset; a
past exercise date is a right not used; the SOFR swaption's closed form is
its script's Monte-Carlo; the exposure engine sees nothing in the past;
what cannot be held is refused at booking.
"""

from __future__ import annotations

import math
from datetime import date, timedelta

import pytest

import quantmodeling as qm
from api.app import portfolio_scripts as ps
from api.app import portfolio_valuation as pv
from api.app import portfolio_xva
from api.app.portfolio_schemas import DerivativeSpec, Instrument
from api.tests.test_portfolio_valuation import CLOSES, END, market  # noqa: F401

TRADE = date(2026, 6, 3)  # a Wednesday


def _mark(spec, d=END):
    return pv.mark(
        Instrument(id="x", spec=spec), d, pv.MarketData(date(2026, 1, 5)), today=END
    )


# ── booking ──────────────────────────────────────────────────────────────────


def test_a_booked_script_keeps_the_contracts_dates():
    spec = ps.book("european-call", {"strike": 1.1}, TRADE, "aaa", "EUR")
    assert TRADE <= spec.start <= TRADE + timedelta(days=6)
    assert spec.start.weekday() < 5 and spec.expiry.weekday() < 5
    assert (spec.expiry - spec.start).days in (364, 365, 366)
    assert spec.underlying == "AAA" and spec.terms == {"strike": 1.1}
    assert spec.start.isoformat() in spec.script  # its dates are written in


def test_what_a_portfolio_cannot_hold_is_refused():
    with pytest.raises(ps.BookingError, match="several underlyings"):
        ps.book("basket-call", {}, TRADE, "AAA")
    with pytest.raises(ps.BookingError, match="ticker"):
        ps.book("european-call", {}, TRADE)
    with pytest.raises(ps.BookingError, match="rates product"):
        ps.book("interest-rate-swap", {}, TRADE, "AAA")


# ── equity marks ─────────────────────────────────────────────────────────────


def _vanilla(**kw) -> DerivativeSpec:
    params = {
        "spot": 1.0,
        "strike": 105.0,
        "maturity": 1.0,
        "rate": 0.02,
        "dividend": 0.0,
        "vol": 0.2,
        "is_call": True,
        "engine": "analytic",
        **kw,
    }
    return DerivativeSpec(
        product="vanilla",
        underlying="AAA",
        currency="EUR",
        expiry=date(2027, 3, 19),
        params=params,
    )


def test_a_converted_position_marks_as_its_hand_written_pricer_did():
    hand = _vanilla()
    script = ps.from_hand_written(hand, TRADE)
    assert script.kind == "script" and "max(spot() - 105.0, 0)" in script.script
    assert _mark(script).value == pytest.approx(_mark(hand).value, rel=1e-12)


def test_the_library_european_once_struck_is_the_vanilla_of_its_strike():
    spec = ps.book("european-call", {"strike": 1.05}, TRADE, "AAA", "EUR")
    s0 = CLOSES["AAA"][spec.start]
    # (the fake market stores no curve: both keep their default rate, 0)
    vanilla = _vanilla(strike=1.05 * s0, rate=0.0).model_copy(
        update={"expiry": spec.expiry}
    )
    m = _mark(spec)
    assert m.value == pytest.approx(_mark(vanilla).value, rel=1e-12)
    assert m.model.params[0].name == "initial level s0"


def test_flows_already_paid_stay_in_the_mark():
    # 1 paid on 2026-07-01, the stock at the end: on END the coupon is paid
    script = "2026-06-01\n    s0 = spot()\n\n2026-07-01\n    pays 1\n\n2027-01-04\n    pays spot()\n"
    spec = ps.ScriptSpec(
        product="custom",
        title="coupon then stock",
        script=script,
        underlying="AAA",
        currency="EUR",
        start=date(2026, 6, 1),
        expiry=date(2027, 1, 4),
    )
    m = _mark(spec)
    paid = {p.name: p.value for p in m.model.params}["flows already paid"]
    assert paid == 1.0
    # what is left is the stock delivered later: about the spot, plus the 1
    spot = CLOSES["AAA"][END]
    assert m.value == pytest.approx(spot + 1.0, rel=0.03)


def test_a_past_exercise_date_is_a_right_not_used():
    spec = ps.book("american-put", {"strike": 1.0}, TRADE, "AAA", "EUR")
    m = _mark(spec)  # weekly exercise dates since June are in the past on END
    assert m.value is not None and m.value > 0, m.note


# ── rates marks ──────────────────────────────────────────────────────────────

FLAT = 0.04
TIMES = [0.25, 0.5, 1, 2, 3, 5, 7, 10, 15, 30]


class _History:
    """A flat 4 % SOFR curve every day, Hull-White (0.05, 1 %)."""

    def curve(self, ccy, d):
        return d, TIMES, [math.exp(-FLAT * t) for t in TIMES]

    def hull_white(self, ccy, obs):
        return 0.05, 0.01


@pytest.fixture
def sofr(monkeypatch):
    real = pv.MarketData.__init__

    def init(self, *a, **k):
        real(self, *a, **k)
        self.rates_history = _History()

    monkeypatch.setattr(pv.MarketData, "__init__", init)
    ps._PAID.clear()


def test_a_swaps_mark_does_not_jump_on_a_reset(sofr):
    spec = ps.book(
        "interest-rate-swap", {"fixed_rate": 0.04}, date(2025, 1, 6), currency="USD"
    )
    reset = ps.event_dates(spec.script, spec.start)[1]  # the second coupon's fixing
    before, after = _mark(spec, reset - timedelta(days=1)), _mark(spec, reset)
    # the coupon fixed on the reset leaves the future and enters what was paid
    assert abs(after.value - before.value) < 1e-5 * spec.terms["notional"]
    paid = {p.name: p.value for p in after.model.params}.get("flows already paid")
    assert paid is not None


def test_the_swaptions_closed_form_is_its_scripts_monte_carlo(sofr):
    spec = ps.book("european-swaption", {"strike": 0.04}, date(2026, 3, 2))
    d = date(2026, 4, 1)  # after the terms' date, before expiry
    m = _mark(spec, d)
    script, _ = ps.season(spec.script, d)
    fixings = {
        e.isoformat(): [1.0] for e in ps.event_dates(spec.script, spec.start) if e <= d
    }
    mc = qm.price_script(
        script,
        1.0,
        0.0,
        0.0,
        1e-12,
        d.isoformat(),
        n_paths=100_000,
        seed=3,
        model="hull_white",
        hull_white={"mean_reversion": 0.05, "sigma": 0.01, "rho": 0.0},
        curve_times=TIMES,
        curve_dfs=[math.exp(-FLAT * t) for t in TIMES],
        historical_fixings=fixings,
    )
    assert abs(m.value - mc["npv"]) < 4 * mc["mc_std_error"]


def test_the_exposure_engine_sees_nothing_in_the_past(sofr):
    spec = ps.book("interest-rate-swap", {"fixed_rate": 0.045}, date(2025, 1, 6))
    d = date(2026, 3, 2)
    script = ps.for_exposure(spec.script, d)
    events = qm.validate_script(script, d.isoformat())["events"]
    assert all(date.fromisoformat(e["date"]) > d for e in events)
    assert "fixed_rate = 0.045" in script  # the terms carried forward
    inst = Instrument(id="s", label="swap", spec=spec)
    made, note, option = portfolio_xva._script_trade(inst, spec, 1.0, d)
    assert made.kind == "script" and note is None and option is None


def test_a_converted_european_stays_an_equity_option_for_the_xva():
    hand = _vanilla().model_copy(update={"currency": "USD"})
    spec = ps.from_hand_written(hand, TRADE)
    made, note, option = portfolio_xva._script_trade(
        Instrument(id="o", spec=spec), spec, 1.0, END
    )
    assert made.kind == "equity_option" and option == hand
