"""At-the-money FX volatilities from real trades — the market input the
two-currency xVA model takes its exchange-rate volatility from
(blueprint/wp/23-xva.md §14.15, lot X10).

Nobody publishes implied FX volatilities for free, but every FX option traded
by a US person is published, premium included, by DTCC's swap data repository
(CFTC public dissemination); `data-ingest` stores the vanilla ones
(`fx.dtcc_options`). This module turns them into a term structure of
at-the-money volatilities.

**Straddles, because they need no spot.** An option's price says little
about volatility without the forward it was struck against, and the forward
is not in the file: the only spot in the store is one fixing a day, while
the rate moves by half a percent in a session — as much as the distance to
the money of an at-the-money option. Read against the fixing, a three-month
option at the money looks half a standard deviation in or out of it, and its
volatility comes out at 4 % or 8.5 % for a market at 6 %.

A call and a put with the same strike and expiry traded together carry their
own forward: by put-call parity,

    C − P = DF_d(T) (F − K)   ⇒   F = K + (C − P) / DF_d(T),

and with it the volatility is the one that reprices either leg (Garman &
Kohlhagen 1983, written on the forward). The median per expiry bucket is the
term structure, with the number of straddles behind each point.

The rules, found on the real files (data-ingest's dtcc-fx-options):

- only new trades with a premium and uncapped amounts (db.dtcc_fx_option_trades);
- **a straddle** is a call and a put on the same trade date, expiry and
  strike, executed within a minute of each other;
- **which currency is called** is read on the amounts exchanged at exercise,
  not on the product's name; the strike must be the ratio of the two amounts
  when both are published (a few rows name the pair the wrong way round);
- **one trade can be reported several times**, by the venue and by a clearing
  or reporting entity: same execution time, expiry, strike and premium count
  once;
- **at the money**: the strike within a quarter of a standard deviation of
  the forward the pair implies, at `MONEYNESS_VOL`. Further out the
  volatility is the smile's, not the level's;
- the premium is taken as paid up front, in either currency of the pair; one
  in the base currency is converted at the day's fixing, where half a
  percent of error on the spot is half a percent on the price.

What this is not: a dealer's surface. Trades are short-dated — most expire
within three months, few beyond a year — and the files do not say whether an
option is European (the convention for OTC vanilla FX options).
"""

from __future__ import annotations

import math
import statistics
from collections import defaultdict
from dataclasses import dataclass
from datetime import date, datetime
from typing import Callable, Dict, List, Mapping, Optional, Sequence, Tuple

from .db import FxOptionTrade

#: Option expiries of the term structure, in years.
EXPIRIES: Tuple[float, ...] = (1 / 12, 0.25, 0.5, 1.0, 2.0)
#: A trade belongs to a bucket if its expiry is within this fraction of it.
EXPIRY_TOLERANCE = 0.2
#: The volatility at which "a quarter of a standard deviation" is measured.
MONEYNESS_VOL = 0.08
MONEYNESS_DEVIATIONS = 0.25
#: The strike must be the ratio of the two amounts this closely.
STRIKE_TOLERANCE = 0.01
#: The two legs of a straddle are executed within this many seconds.
PAIR_SECONDS = 60.0
#: Fewer straddles than this is not a quote.
MIN_TRADES = 3
#: Outside this range an implied volatility is a reporting error.
VOL_BOUNDS = (0.02, 0.40)

_DAYS_PER_YEAR = 365.25


@dataclass(frozen=True)
class FxVolPoint:
    expiry: float
    volatility: float  # median
    low: float  # lower quartile
    high: float  # upper quartile
    trades: int  # straddles


@dataclass(frozen=True)
class FxVolGrid:
    points: List[FxVolPoint]
    #: Why trades were left out: reason -> count.
    rejected: Dict[str, int]
    #: Straddles behind the points.
    trades_used: int


def black(
    forward: float, strike: float, expiry: float, vol: float, call: bool
) -> float:
    """Undiscounted Black price of an option on the forward."""
    sd = vol * math.sqrt(expiry)
    d1 = math.log(forward / strike) / sd + 0.5 * sd
    d2 = d1 - sd

    def cdf(x: float) -> float:
        return 0.5 * math.erfc(-x / math.sqrt(2.0))

    if call:
        return forward * cdf(d1) - strike * cdf(d2)
    return strike * cdf(-d2) - forward * cdf(-d1)


def implied_volatility(
    price: float, forward: float, strike: float, expiry: float, call: bool
) -> Optional[float]:
    """The volatility at which black() gives `price`; None when no volatility
    in (0.1 %, 200 %) does — a price under the intrinsic value, or absurd."""
    lo, hi = 0.001, 2.0
    if (
        not black(forward, strike, expiry, lo, call)
        < price
        < black(forward, strike, expiry, hi, call)
    ):
        return None
    for _ in range(80):
        mid = 0.5 * (lo + hi)
        if black(forward, strike, expiry, mid, call) > price:
            hi = mid
        else:
            lo = mid
    return 0.5 * (lo + hi)


def _bucket(expiry: float) -> Optional[float]:
    nearest = min(EXPIRIES, key=lambda b: abs(b - expiry))
    return nearest if abs(nearest - expiry) / nearest <= EXPIRY_TOLERANCE else None


def atm_volatilities(
    trades: Sequence[FxOptionTrade],
    base: str,
    quote: str,
    spots: Mapping[date, float],
    discount_quote: Callable[[float], float],
) -> FxVolGrid:
    """The term structure of ATM volatilities of BASE/QUOTE implied by the
    straddles among `trades`. `spots` gives the fixing (QUOTE per BASE) of
    each trade date, used only to convert a premium paid in BASE;
    `discount_quote` the QUOTE discount factor to a maturity."""
    rejected: Dict[str, int] = defaultdict(int)
    seen = set()
    # (trade date, expiry, strike) -> the legs: (executed, is a call on BASE,
    # price in QUOTE per unit of BASE).
    legs: Dict[tuple, List[Tuple[Optional[datetime], bool, float]]] = defaultdict(list)
    for t in trades:
        key = (t.executed, t.expiry, t.strike, round(t.premium, 2), t.premium_currency)
        if key in seen:
            rejected["the same trade reported again"] += 1
            continue
        seen.add(key)

        expiry = (t.expiry - t.trade_date).days / _DAYS_PER_YEAR
        if expiry <= 0 or _bucket(expiry) is None:
            rejected["off the grid of expiries"] += 1
            continue

        amounts = {t.call_currency: t.call_amount, t.put_currency: t.put_amount}
        base_amount, quote_amount = amounts.get(base), amounts.get(quote)
        if base_amount and quote_amount:
            if abs(quote_amount / base_amount / t.strike - 1.0) > STRIKE_TOLERANCE:
                rejected["strike that is not the ratio of the amounts"] += 1
                continue
        elif quote_amount:
            base_amount = quote_amount / t.strike
        if not base_amount:
            rejected["no amount published"] += 1
            continue
        # Which currency is called: the amounts say it; the product's name
        # (about the pair's first currency) only when they are missing.
        if t.call_currency in (base, quote):
            call = t.call_currency == base
        elif t.put_currency in (base, quote):
            call = t.put_currency != base
        else:
            call = t.option_type == "call"

        if t.premium_currency == quote:
            premium = t.premium
        elif t.premium_currency == base:
            spot = spots.get(t.trade_date)
            if spot is None:
                rejected["no spot fixing to convert the premium"] += 1
                continue
            premium = t.premium * spot
        else:
            rejected["premium in a third currency"] += 1
            continue
        legs[(t.trade_date, t.expiry, t.strike)].append(
            (t.executed, call, premium / base_amount)
        )

    vols: Dict[float, List[float]] = defaultdict(list)
    for (trade_date, expiry_date, strike), members in legs.items():
        calls = [m for m in members if m[1]]
        puts = [m for m in members if not m[1]]
        expiry = (expiry_date - trade_date).days / _DAYS_PER_YEAR
        bucket = _bucket(expiry)
        discount = discount_quote(expiry)
        used = set()
        for executed, _, call_price in calls:
            # The put traded with it: the closest in time, within a minute.
            best = None
            for j, (other, _, _) in enumerate(puts):
                if j in used or executed is None or other is None:
                    continue
                gap = abs((executed - other).total_seconds())
                if gap <= PAIR_SECONDS and (best is None or gap < best[0]):
                    best = (gap, j)
            if best is None:
                rejected["not one leg of a straddle"] += 1
                continue
            used.add(best[1])
            put_price = puts[best[1]][2]
            forward = strike + (call_price - put_price) / discount
            if forward <= 0 or abs(math.log(strike / forward)) > (
                MONEYNESS_DEVIATIONS * MONEYNESS_VOL * math.sqrt(expiry)
            ):
                rejected["straddle struck away from the money"] += 1
                continue
            vol = implied_volatility(
                call_price / discount, forward, strike, expiry, True
            )
            if vol is None or not VOL_BOUNDS[0] <= vol <= VOL_BOUNDS[1]:
                rejected["implausible premium"] += 1
                continue
            vols[bucket].append(vol)
        rejected["not one leg of a straddle"] += len(puts) - len(used)

    points: List[FxVolPoint] = []
    count = 0
    for bucket, values in sorted(vols.items()):
        if len(values) < MIN_TRADES:
            rejected["bucket with too few straddles"] += len(values)
            continue
        low, median, high = statistics.quantiles(values, n=4, method="inclusive")
        points.append(FxVolPoint(bucket, median, low, high, len(values)))
        count += len(values)
    return FxVolGrid(
        points=points,
        rejected={k: v for k, v in rejected.items() if v},
        trades_used=count,
    )
