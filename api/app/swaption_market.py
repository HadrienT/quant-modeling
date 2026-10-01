"""ATM swaption normal vols from real trades — the market input the /rates
page and the xVA work calibrate Hull-White on.

Nobody publishes a swaption volatility surface for free, but every swaption
traded by a US person is published, price included, by DTCC's swap data
repository (CFTC public dissemination); `data-ingest` stores those messages
and the day's SOFR par swap curve built from the swaps of the same file
(`rates.dtcc_swaptions`, `rates.dtcc_swap_rates`). This module turns them
into a grid of at-the-money normal vols:

    premium / notional = annuity × Bachelier(forward, strike, expiry, σ)

inverted for σ, trade by trade, on the swap curve of the trade's own day;
then the median per (expiry, tenor) bucket over a rolling window, with the
number of trades behind each point.

The rules, found on the real files (blueprint/wp/23-xva.md §13.7, issue #137):

- only new trades with a full notional and a premium (db.dtcc_swaption_trades);
- **a straddle traded on a platform carries the premium of both legs on each
  leg**: a call and a put with the same dates, strike, notional and premium
  on a platform other than a bilateral one count for half the premium each.
  Left alone they come out at twice the vol;
- **at the money, whatever "call" means**: the files do not say whether a
  call is a payer or a receiver swaption. A trade is kept only if the two
  readings give the same vol within `READING_TOLERANCE` — which is the case
  when the strike is at the forward, where payer and receiver are worth the
  same;
- **expiries up to two years**: beyond, premiums look paid at expiry rather
  than up front, which the files do not say either, and the vols would be
  overstated by the discount factor to the expiry.

What this is not: a dealer's surface. It is the median of a handful of trades
per point, on a curve that is itself a median of trades; the count and the
quartiles are returned so that the reader can see what each number rests on.
"""

from __future__ import annotations

import statistics
from collections import defaultdict
from dataclasses import dataclass
from datetime import date
from typing import Dict, Iterable, List, Mapping, Optional, Sequence, Tuple

import quantmodeling as qm

from .db import SwapRate, SwaptionTrade

#: The grid: option expiries and swap tenors, in years.
EXPIRIES: Tuple[float, ...] = (0.25, 0.5, 1.0, 2.0)
TENORS: Tuple[int, ...] = (1, 2, 5, 10, 30)
#: A trade belongs to a bucket if its expiry is within this fraction of the
#: bucket's and its tenor within TENOR_TOLERANCE years.
EXPIRY_TOLERANCE = 0.2
TENOR_TOLERANCE = 0.1
#: Payer and receiver readings of the same premium must agree this closely.
READING_TOLERANCE = 0.05
#: Fewer trades than this is not a quote.
MIN_TRADES = 3
#: Outside this range an implied normal vol is a reporting error (wrong
#: notional, premium in another unit), not a market level.
VOL_BOUNDS = (0.0010, 0.0400)
#: Platforms where a trade is bilateral: its premium is the leg's own.
BILATERAL = frozenset({"BILT", "XXXX", None, ""})

_DAYS_PER_YEAR = 365.25


@dataclass(frozen=True)
class VolPoint:
    expiry: float
    tenor: int
    normal_vol: float  # median
    low: float  # lower quartile
    high: float  # upper quartile
    trades: int


@dataclass(frozen=True)
class VolGrid:
    points: List[VolPoint]
    #: Why trades were left out: reason -> count.
    rejected: Dict[str, int]
    trades_used: int


def _bucket(
    value: float, buckets: Sequence[float], tolerance: float, relative: bool
) -> Optional[float]:
    nearest = min(buckets, key=lambda b: abs(b - value))
    gap = abs(nearest - value) / nearest if relative else abs(nearest - value)
    return nearest if gap <= tolerance else None


def _straddle_legs(trades: Iterable[SwaptionTrade]) -> set:
    """Indices of the trades that are one leg of a platform straddle."""
    groups: Dict[tuple, List[int]] = defaultdict(list)
    listed = list(trades)
    for i, t in enumerate(listed):
        if t.platform in BILATERAL:
            continue
        groups[
            (
                t.report_date,
                t.expiry,
                t.underlier_maturity,
                t.strike,
                t.notional,
                t.premium,
            )
        ].append(i)
    legs = set()
    for members in groups.values():
        if {listed[i].option_type for i in members} >= {"call", "put"}:
            legs.update(members)
    return legs


class _Curve:
    """The OIS curve of one day, bootstrapped from its par swap rates."""

    def __init__(self, rates: Sequence[SwapRate]):
        curve = qm.bootstrap_ois_curve(
            [], [(float(r.tenor_years), r.rate) for r in rates]
        )
        self.times, self.dfs = curve["times"], curve["discount_factors"]
        self.last = float(rates[-1].tenor_years)

    def forward_swap(self, start: float, tenor: int) -> Tuple[float, float]:
        """(forward par rate, annuity) of the annual swap starting at `start`."""
        swap = qm.price_swap(
            self.times,
            self.dfs,
            self.times,
            self.dfs,
            start,
            float(tenor),
            0.0,
            1,
            1,
            1.0,
            True,
            0.0,
        )
        return swap["par_rate"], swap["annuity"]


def atm_normal_vols(
    trades: Sequence[SwaptionTrade], curves: Mapping[date, Sequence[SwapRate]]
) -> VolGrid:
    """The grid of ATM normal vols implied by `trades`, each priced on the
    swap curve of its report date (`curves`)."""
    rejected: Dict[str, int] = defaultdict(int)
    straddle = _straddle_legs(trades)
    built: Dict[date, Optional[_Curve]] = {}
    vols: Dict[Tuple[float, int], List[float]] = defaultdict(list)

    for i, trade in enumerate(trades):
        expiry = (trade.expiry - trade.trade_date).days / _DAYS_PER_YEAR
        tenor_years = (trade.underlier_maturity - trade.expiry).days / _DAYS_PER_YEAR
        expiry_bucket = (
            _bucket(expiry, EXPIRIES, EXPIRY_TOLERANCE, relative=True)
            if expiry > 0
            else None
        )
        tenor_bucket = _bucket(tenor_years, TENORS, TENOR_TOLERANCE, relative=False)
        if expiry_bucket is None or tenor_bucket is None:
            rejected["off the grid (expiry beyond two years, odd tenor)"] += 1
            continue

        if trade.report_date not in built:
            rates = curves.get(trade.report_date)
            built[trade.report_date] = (
                _Curve(rates) if rates and len(rates) >= 5 else None
            )
        curve = built[trade.report_date]
        if curve is None or expiry + tenor_bucket > curve.last + 1.0:
            rejected["no swap curve that day"] += 1
            continue

        # A few rows publish the strike as a percentage (4.9 for 0.049).
        strike = trade.strike / 100.0 if trade.strike > 0.3 else trade.strike
        forward, annuity = curve.forward_swap(expiry, int(tenor_bucket))
        price = trade.premium / trade.notional * (0.5 if i in straddle else 1.0)
        payer = qm.bachelier_implied_vol(True, price, forward, strike, expiry, annuity)
        receiver = qm.bachelier_implied_vol(
            False, price, forward, strike, expiry, annuity
        )
        if not (payer == payer and receiver == receiver):  # NaN: below intrinsic
            rejected["away from the money"] += 1
            continue
        vol = 0.5 * (payer + receiver)
        if abs(payer - receiver) > READING_TOLERANCE * vol:
            rejected["away from the money"] += 1
            continue
        if not VOL_BOUNDS[0] <= vol <= VOL_BOUNDS[1]:
            rejected["implausible premium"] += 1
            continue
        vols[(expiry_bucket, int(tenor_bucket))].append(vol)

    points: List[VolPoint] = []
    used = 0
    for (expiry_bucket, tenor_bucket), values in sorted(vols.items()):
        if len(values) < MIN_TRADES:
            rejected["bucket with too few trades"] += len(values)
            continue
        low, median, high = statistics.quantiles(values, n=4, method="inclusive")
        points.append(
            VolPoint(expiry_bucket, tenor_bucket, median, low, high, len(values))
        )
        used += len(values)
    return VolGrid(points=points, rejected=dict(rejected), trades_used=used)
