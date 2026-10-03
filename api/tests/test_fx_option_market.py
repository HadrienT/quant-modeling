"""At-the-money FX volatilities from traded straddles — `fx_option_market.py`.

Trades are made here from a known forward and a known volatility: the grid
must give the volatility back without being told the forward, and each rule
found on the real files is exercised on its own.
"""

from __future__ import annotations

import math
from dataclasses import replace
from datetime import date, datetime, timedelta, timezone
from typing import List

import pytest

from api.app import fx_option_market as market
from api.app.db import FxOptionTrade

DAY = date(2026, 10, 1)
SPOT = 1.1200
VOL = 0.07
RATE = 0.045


def discount(t: float) -> float:
    return math.exp(-RATE * t)


def straddle(
    expiry_years: float,
    *,
    vol: float = VOL,
    forward: float = 1.1300,
    strike_offset: float = 0.0,
    euros: float = 10e6,
    premium_in_euros: bool = False,
    second: int = 0,
    day: date = DAY,
) -> List[FxOptionTrade]:
    """A call and a put on the euro, same strike, executed one second apart,
    priced at `vol` against `forward`."""
    expiry = day + timedelta(days=round(expiry_years * 365.25))
    t = (expiry - day).days / 365.25
    strike = round(forward + strike_offset, 4)
    at = datetime(day.year, day.month, day.day, 9, 0, second, tzinfo=timezone.utc)
    legs = []
    for call in (True, False):
        premium = euros * discount(t) * market.black(forward, strike, t, vol, call)
        legs.append(
            FxOptionTrade(
                report_date=day,
                trade_date=day,
                executed=at + timedelta(seconds=0 if call else 1),
                option_type="call" if call else "put",
                call_currency="EUR" if call else "USD",
                call_amount=euros if call else euros * strike,
                put_currency="USD" if call else "EUR",
                put_amount=euros * strike if call else euros,
                strike=strike,
                expiry=expiry,
                premium=premium / SPOT if premium_in_euros else premium,
                premium_currency="EUR" if premium_in_euros else "USD",
            )
        )
    return legs


def grid(trades, spots=None):
    return market.atm_volatilities(
        trades, "EUR", "USD", {DAY: SPOT} if spots is None else spots, discount
    )


def many(expiry_years: float, **kwargs) -> List[FxOptionTrade]:
    return [
        leg
        for k, euros in enumerate((5e6, 10e6, 25e6))
        for leg in straddle(expiry_years, euros=euros, second=10 * k, **kwargs)
    ]


def test_black_and_its_inverse():
    call = market.black(1.13, 1.10, 0.5, 0.08, True)
    put = market.black(1.13, 1.10, 0.5, 0.08, False)
    assert call - put == pytest.approx(1.13 - 1.10)  # put-call parity
    assert market.implied_volatility(call, 1.13, 1.10, 0.5, True) == pytest.approx(0.08)
    assert market.implied_volatility(put, 1.13, 1.10, 0.5, False) == pytest.approx(0.08)
    # Under the intrinsic value no volatility prices it.
    assert market.implied_volatility(0.02, 1.13, 1.10, 0.5, True) is None


def test_the_vol_comes_back_without_being_told_the_forward():
    # The forward the trades were priced against, 1.13, is nowhere in the
    # inputs: the only spot given is a fixing 0.9 % away from it.
    g = grid(many(0.25) + many(1.0, vol=0.065))
    assert [(p.expiry, p.trades) for p in g.points] == [(0.25, 3), (1.0, 3)]
    assert g.points[0].volatility == pytest.approx(VOL, rel=1e-6)
    assert g.points[1].volatility == pytest.approx(0.065, rel=1e-6)
    assert g.trades_used == 6 and g.rejected == {}
    # Read against the fixing instead, the same options would look half a
    # standard deviation in or out of the money: that is the bias avoided.
    call = many(0.25)[0]
    t = (call.expiry - DAY).days / 365.25
    naive = market.implied_volatility(
        call.premium / call.call_amount / discount(t), SPOT, call.strike, t, True
    )
    assert naive > 1.1 * VOL


def test_a_premium_paid_in_euros_is_converted_at_the_fixing():
    g = grid(many(0.5, premium_in_euros=True))
    assert g.points[0].volatility == pytest.approx(VOL, rel=1e-6)
    # Without a fixing that day the premium cannot be read.
    g = grid(many(0.5, premium_in_euros=True), spots={})
    assert g.points == [] and g.rejected == {"no spot fixing to convert the premium": 6}


def test_the_point_is_the_median_and_resists_a_bad_print():
    trades = [
        leg
        for k, vol in enumerate((0.066, 0.069, 0.070, 0.071, 0.150))
        for leg in straddle(1.0, vol=vol, second=10 * k)
    ]
    (point,) = grid(trades).points
    assert point.volatility == pytest.approx(0.070, rel=1e-6)
    assert point.low == pytest.approx(0.069, rel=1e-6)
    assert point.high == pytest.approx(0.071, rel=1e-6)
    assert point.trades == 5


def test_only_straddles_at_the_money_are_read():
    atm = many(0.25)
    # A call alone, a put executed ten minutes after its call, and a straddle
    # struck 3 % above the forward.
    alone = straddle(0.25, euros=7e6, second=40)[:1]
    late = straddle(0.25, euros=8e6, second=50)
    late[1] = replace(late[1], executed=late[1].executed + timedelta(minutes=10))
    far = straddle(0.25, strike_offset=0.035, second=30)
    g = grid(atm + alone + far)
    assert g.points[0].trades == 3
    assert g.rejected == {
        "not one leg of a straddle": 1,
        "straddle struck away from the money": 1,
    }
    g = grid(
        atm
        + [
            replace(
                leg,
                strike=1.1295,
                call_amount=None,
                put_amount=None,
                call_currency=None,
                put_currency=None,
            )
            for leg in late
        ]
    )
    assert g.rejected == {"no amount published": 2}


def test_the_same_trade_reported_twice_counts_once():
    trades = many(0.25)
    again = [replace(leg, report_date=DAY + timedelta(days=1)) for leg in trades[:2]]
    g = grid(trades + again)
    assert g.points[0].trades == 3
    assert g.rejected == {"the same trade reported again": 2}


def test_the_amounts_say_which_currency_is_called_and_check_the_strike():
    trades = many(0.25)
    # The product's name says "put" on every row: the amounts are believed.
    renamed = [replace(leg, option_type="put") for leg in trades]
    assert grid(renamed).points[0].volatility == pytest.approx(VOL, rel=1e-6)
    # Only the dollar amount published: the euros are that amount over the strike.
    one_sided = [
        (
            replace(leg, call_currency=None, call_amount=None)
            if leg.call_currency == "EUR"
            else replace(leg, put_currency=None, put_amount=None)
        )
        for leg in trades
    ]
    assert grid(one_sided).points[0].volatility == pytest.approx(VOL, rel=1e-4)
    # A strike that is not the ratio of the two amounts is a reporting error.
    wrong = [replace(leg, strike=leg.strike * 1.05) for leg in trades]
    assert grid(wrong).rejected == {"strike that is not the ratio of the amounts": 6}


def test_off_the_grid_too_few_or_absurd_give_no_point():
    assert grid(many(0.75)).rejected == {"off the grid of expiries": 6}
    g = grid(straddle(1.0) + straddle(1.0, second=10))
    assert g.points == [] and g.rejected == {"bucket with too few straddles": 2}
    # 80 % of volatility on EUR/USD is a premium in the wrong unit.
    assert grid(many(1.0, vol=0.80)).rejected == {"implausible premium": 3}
