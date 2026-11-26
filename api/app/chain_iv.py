"""
The quotes data-ingest cleaned at ingestion (options.chain_iv, WP 31), made
ready for the vol surface.

data-ingest computes, per quote, our own Black-76 implied vol on the quote's
European price against its expiry's forward F and discount DF (parity on a
European chain, carry on an American one, whose mids it de-Americanizes), and
flags what fails a data-quality check instead of dropping it. This module is
the consumer's half of that contract: which flags exclude a quote (WP 31
§6.1, decision D4), the two-zero-bid stop of the Cboe VIX strip, and decision
D2: S&P 500 products are priced on the European SPX surface, carried onto
the product's own forwards.

Why the SPX surface transfers: a surface in k = ln(K / F) and T describes the
index's distribution relative to its forward. SPY is the index less its
dividends and fees, which sit in SPY's forward, not in the smile. Black's
price is homogeneous of degree one in (F, K, price), so scaling an SPX
quote's strike, bid and ask by F_SPY(T) / F_SPX(T) leaves its implied vol and
its k unchanged and puts it on SPY's forward: the same surface, at SPY's
scale.
"""

from __future__ import annotations

import bisect
import enum
import math
from dataclasses import dataclass
from typing import Dict, List, Optional, Sequence, Tuple

import quantmodeling as qm

from . import db


class Flag(enum.IntFlag):
    """Bits of options.chain_iv.flags, as data-ingest's docs/sources.md
    defines them (a bit's meaning never changes there)."""

    NO_BID = 1
    WIDE_SPREAD = 2
    BELOW_TWO_TICKS = 4
    STALE = 8
    PARITY_OUTLIER = 16
    EXPIRY_REJECTED = 32
    IV_FAILED = 64
    NOT_OTM = 128
    SHORT_MATURITY = 256
    RATE_FALLBACK = 512


#: What excludes a quote from the surface (WP 31 §6.1). Not in the set:
#: STALE, wrong on a holiday's snapshot (data-ingest#34), and RATE_FALLBACK,
#: reported as a warning instead.
EXCLUDED = (
    Flag.NO_BID
    | Flag.WIDE_SPREAD
    | Flag.BELOW_TWO_TICKS
    | Flag.PARITY_OUTLIER
    | Flag.EXPIRY_REJECTED
    | Flag.IV_FAILED
    | Flag.NOT_OTM
    | Flag.SHORT_MATURITY
)

#: After this many consecutive zero bids walking away from the forward, no
#: further strike of that side is used (the Cboe VIX strip's rule).
ZERO_BID_STOP = 2

#: The tickers priced on a European index chain's surface (decision D2), and
#: that chain's ticker in options.chain_iv.
EUROPEAN_PROXY: Dict[str, str] = {"SPY": "^SPX"}


def usable(rows: Sequence["db.ChainIvRow"]) -> List["db.ChainIvRow"]:
    """The rows the surface may read: OTM, unflagged by EXCLUDED, and short
    of the two-zero-bid stop on their side of their expiry."""
    cut = set()
    sides: Dict[Tuple, List["db.ChainIvRow"]] = {}
    for r in rows:
        if not r.flags & Flag.NOT_OTM:
            sides.setdefault((r.expiry, r.root, r.option_type), []).append(r)
    for (_, _, option_type), side in sides.items():
        # Away from the forward: puts downwards in strike, calls upwards.
        side.sort(key=lambda r: r.strike, reverse=option_type == "put")
        zeros = 0
        for i, r in enumerate(side):
            zeros = zeros + 1 if r.flags & Flag.NO_BID else 0
            if zeros >= ZERO_BID_STOP:
                cut.update(id(x) for x in side[i + 1 :])
                break
    return [
        r for r in rows if not r.flags & EXCLUDED and id(r) not in cut and r.ttm > 0
    ]


def quote(
    r: "db.ChainIvRow", scale: float = 1.0, forward: Optional[float] = None
) -> "qm.RawOptionQuote":
    """The native quote of a row, its strike and prices scaled by `scale`
    onto `forward` (D2); scale 1 keeps the row's own."""
    q = qm.RawOptionQuote()
    q.strike = r.strike * scale
    q.ttm = r.ttm
    q.is_call = r.option_type == "call"
    q.bid = (r.bid or 0.0) * scale
    q.ask = (r.ask or 0.0) * scale
    q.last = (r.mid or 0.0) * scale
    iv = r.implied_vol
    q.has_iv = iv is not None and iv == iv and iv > 0.0
    q.implied_vol = float(iv) if q.has_iv else 0.0
    q.forward = forward if forward is not None else r.forward
    q.discount = r.discount
    return q


def row_key(r: "db.ChainIvRow") -> tuple:
    """What a row contributes to the surface: what a digest of the rows
    hashes when the registry has no checksum."""
    return (
        r.expiry.isoformat(),
        r.root,
        r.option_type,
        r.strike,
        r.ttm,
        r.bid,
        r.ask,
        r.forward,
        r.discount,
        r.implied_vol,
        r.flags,
    )


@dataclass(frozen=True)
class CarryCurve:
    """A ticker's market forwards as a carry rate c(T) = ln(F(T) / S) / T,
    linear in T between its expiries and flat beyond them, from its accepted
    options.chain_iv_expiries."""

    spot: float
    ttms: Tuple[float, ...]
    carries: Tuple[float, ...]

    @staticmethod
    def of(expiries: Sequence["db.ChainIvExpiry"], spot: float) -> "CarryCurve":
        points: Dict[float, float] = {}
        for e in expiries:
            s = e.spot if e.spot else spot
            points.setdefault(e.ttm, math.log(e.forward / s) / e.ttm)
        ttms = tuple(sorted(points))
        if not ttms:
            raise ValueError("no accepted expiry to read the forwards from")
        return CarryCurve(spot, ttms, tuple(points[t] for t in ttms))

    def carry(self, ttm: float) -> float:
        i = bisect.bisect_left(self.ttms, ttm)
        if i == 0:
            return self.carries[0]
        if i == len(self.ttms):
            return self.carries[-1]
        t0, t1 = self.ttms[i - 1], self.ttms[i]
        c0, c1 = self.carries[i - 1], self.carries[i]
        return c0 + (c1 - c0) * (ttm - t0) / (t1 - t0)

    def forward(self, ttm: float) -> float:
        return self.spot * math.exp(self.carry(ttm) * ttm)


def carried_onto(
    proxy_rows: Sequence["db.ChainIvRow"], target: CarryCurve
) -> List["qm.RawOptionQuote"]:
    """The proxy's usable quotes carried onto the target's forwards (D2):
    each scaled by F_target(T) / F_proxy(T), its implied vol, k and discount
    unchanged."""
    out = []
    for r in usable(proxy_rows):
        f = target.forward(r.ttm)
        out.append(quote(r, f / r.forward, f))
    return out


@dataclass(frozen=True)
class ZeroCurve:
    """A ticker's market discount as a zero rate z(T) = -ln DF(T) / T,
    linear in T between its expiries and flat beyond them."""

    ttms: Tuple[float, ...]
    zeros: Tuple[float, ...]

    @staticmethod
    def of(expiries: Sequence["db.ChainIvExpiry"]) -> "ZeroCurve":
        points: Dict[float, float] = {}
        for e in expiries:
            points.setdefault(e.ttm, -math.log(e.discount) / e.ttm)
        ttms = tuple(sorted(points))
        if not ttms:
            raise ValueError("no accepted expiry to read the discounts from")
        return ZeroCurve(ttms, tuple(points[t] for t in ttms))

    def zero(self, ttm: float) -> float:
        i = bisect.bisect_left(self.ttms, ttm)
        if i == 0:
            return self.zeros[0]
        if i == len(self.ttms):
            return self.zeros[-1]
        t0, t1 = self.ttms[i - 1], self.ttms[i]
        z0, z1 = self.zeros[i - 1], self.zeros[i]
        return z0 + (z1 - z0) * (ttm - t0) / (t1 - t0)


@dataclass(frozen=True)
class MarketCarry:
    """The flat (rate, dividend yield) a product is priced at (WP 31 L6, D1):
    fitted to the market's discount and forward at its horizon T, so that a
    vanilla expiring at T reprices on the surface's forward under every
    model. A typed rate replaces the discount only: the forward stays the
    market's, so the yield becomes rate - carry."""

    horizon: float
    market_rate: float  # -ln DF(T) / T
    carry: float  # ln(F(T) / S) / T
    rate: float
    dividend: float
    typed: bool

    @staticmethod
    def at(
        expiries: Sequence["db.ChainIvExpiry"],
        spot: float,
        horizon: float,
        typed_rate: Optional[float],
    ) -> "MarketCarry":
        c = CarryCurve.of(expiries, spot).carry(horizon)
        z = ZeroCurve.of(expiries).zero(horizon)
        r = z if typed_rate is None else float(typed_rate)
        return MarketCarry(horizon, z, c, r, r - c, typed_rate is not None)
