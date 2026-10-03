"""The second currency of the xVA page — `xva_fx.py`, on a fake store.

The EUR curve and its Hull-White calibration are those of the USD side, read
on another product; what is specific is tested here: the correlations, the
calibration of the spot volatility on traded straddles, and the explicit
errors when an input is missing.
"""

from __future__ import annotations

import math
from datetime import date, timedelta

import pandas as pd
import pytest

from api.app import db, fx, xva_fx
from api.tests.test_fx_option_market import DAY, SPOT, discount, many

USD_HW = (0.04, 0.012)
TIMES = [1.0, 2.0, 5.0, 10.0, 30.0]
USD_DFS = [discount(t) for t in TIMES]
EUR_DFS = [math.exp(-0.03 * t) for t in TIMES]
#: The last one is the Friday before the day of the market.
FRIDAYS = [date(2023, 9, 29) + timedelta(weeks=k) for k in range(157)]


def noise(k: int, salt: float) -> float:
    x = math.sin(12.9898 * k + salt) * 43758.5453
    return 2.0 * (x - math.floor(x)) - 1.0


def weekly_changes(rho: float):
    """Three years of weekly moves: a dollar rate, a euro rate that follows it
    with correlation about `rho`, and an exchange rate independent of both."""
    usd, eur, spot = [4.0], [3.0], [1.10]
    for k in range(1, len(FRIDAYS)):
        a, b = noise(k, 0.0), noise(k, 1.7)
        usd.append(usd[-1] + 0.10 * a)
        eur.append(eur[-1] + 0.10 * (rho * a + math.sqrt(1.0 - rho * rho) * b))
        spot.append(spot[-1] * math.exp(0.01 * noise(k, 4.1)))
    return usd, eur, spot


@pytest.fixture
def store(monkeypatch):
    state = {"straddles": many(0.25, vol=0.06)}
    usd, eur, spot = weekly_changes(0.7)
    state["spot"] = pd.Series(spot + [SPOT], index=FRIDAYS + [DAY])

    monkeypatch.setattr(
        xva_fx,
        "_foreign_rates",
        lambda as_of: xva_fx.ForeignRates(
            as_of, "EUR Euribor, test", TIMES, EUR_DFS, 0.04, 0.010, 4.0, 9, 90
        ),
    )
    monkeypatch.setattr(
        fx,
        "pair_history",
        lambda base, quote, since=None: state["spot"][
            [since is None or d >= since for d in state["spot"].index]
        ],
    )
    monkeypatch.setattr(
        db, "dtcc_fx_option_trades", lambda pair, since: list(state["straddles"])
    )
    monkeypatch.setattr(
        db,
        "fred_series",
        lambda series_id, since=None: pd.Series(usd, index=pd.to_datetime(FRIDAYS)),
    )
    monkeypatch.setattr(
        db, "rates_history", lambda table, series_id, since: list(zip(FRIDAYS, eur))
    )
    return state


def year_straddles(count: int, vol: float = 0.07):
    """`count` one-year straddles, each executed at its own second."""
    from api.tests.test_fx_option_market import straddle

    return [leg for k in range(count) for leg in straddle(1.0, vol=vol, second=4 * k)]


def build():
    return xva_fx.market(DAY, TIMES, USD_DFS, USD_HW)


def test_the_spot_volatility_reprices_the_longest_liquid_expiry(store):
    store["straddles"] = many(0.25, vol=0.06) + year_straddles(12)
    m = build()
    assert (m.spot, m.spot_as_of) == (SPOT, DAY)
    # Three straddles at three months, twelve at one year: the year has
    # enough of them to calibrate on.
    assert m.calibration_expiry == 1.0
    by_expiry = {v.expiry: v for v in m.fx_volatilities}
    assert by_expiry[1.0].market == pytest.approx(0.07, rel=1e-6)
    assert by_expiry[1.0].model == pytest.approx(by_expiry[1.0].market, rel=1e-9)
    assert by_expiry[1.0].straddles == 12
    # One number cannot follow a term structure: off the calibration expiry
    # the model has its own volatility, not the market's 6 %.
    # Three months is before the curve's first pillar: the discount factor
    # there is that of a constant forward rate, not the pillar's.
    assert by_expiry[0.25].market == pytest.approx(0.06, rel=1e-6)
    assert by_expiry[0.25].model == pytest.approx(0.07, abs=0.002)
    # The forward carries the rates' volatility too: the spot's is not 7 %.
    assert m.spot_volatility != pytest.approx(0.07, abs=1e-5)
    assert m.spot_volatility == pytest.approx(0.07, abs=0.003)
    argument = m.argument()
    assert argument["spot"] == SPOT and argument["fx_volatility"] == m.spot_volatility
    assert argument["correlations"] == m.correlations()


def test_correlations_are_estimates_with_their_interval(store):
    store["straddles"] = year_straddles(12)
    m = build()
    rates = m.domestic_foreign
    assert rates.value == pytest.approx(0.7, abs=0.08)
    assert rates.low < rates.value < rates.high and rates.weeks == 156
    assert rates.high - rates.low < 0.25
    # The exchange rate was drawn apart from the rates: zero is in the interval.
    for estimate in (m.domestic_fx, m.foreign_fx):
        assert estimate.low < 0.0 < estimate.high
    # The first week of the three-year window has no change before it.
    assert m.correlation_start == FRIDAYS[2] and m.correlation_end == FRIDAYS[-1]


def test_a_missing_input_is_an_explicit_error(store, monkeypatch):
    # Three straddles at the only expiry: not enough to calibrate on.
    with pytest.raises(xva_fx.TwoCurrencyUnavailable, match="at-the-money straddles"):
        build()
    store["straddles"] = year_straddles(12)
    build()
    # A spot that stopped being published.
    fresh = store["spot"]
    store["spot"] = fresh[[d <= DAY - timedelta(days=30) for d in fresh.index]]
    with pytest.raises(xva_fx.TwoCurrencyUnavailable, match="reference rate is from"):
        build()
    store["spot"] = fresh
    # Too short a history for a correlation.
    monkeypatch.setattr(db, "rates_history", lambda table, series_id, since: [])
    with pytest.raises(xva_fx.TwoCurrencyUnavailable, match="No history"):
        build()


def test_the_euro_curve_must_be_that_of_the_day(monkeypatch):
    monkeypatch.undo()
    monkeypatch.setattr(db, "dtcc_swap_curves", lambda product, since: {})
    with pytest.raises(xva_fx.TwoCurrencyUnavailable, match="No Euribor swap curve"):
        xva_fx._foreign_rates(DAY)


def test_par_coupon_makes_a_bond_worth_par():
    coupon = xva_fx.par_coupon(TIMES, EUR_DFS, 10.0)
    years = [float(k) for k in range(1, 11)]
    import quantmodeling as qm

    discounts = qm.rate_curve_discount_factors(TIMES, EUR_DFS, years)
    assert coupon * sum(discounts) + discounts[-1] == pytest.approx(1.0)
