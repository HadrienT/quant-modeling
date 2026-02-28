"""
Stage 2 — Option chain cleaner / arbitrage filter.

Responsibility: receive raw OptionQuote objects and remove anything that
would contaminate the IV surface calibration:
  1. Liquidity filter   — spread, open interest, bid floor
  2. Moneyness filter   — discard deep OTM / deep ITM strikes
  3. Calendar arbitrage — total variance must be non-decreasing in T
  4. Butterfly arbitrage — call price must be convex in K (per expiry)

Public API
----------
CleaningStats
clean_chain(quotes, spot, ...) -> (list[OptionQuote], CleaningStats)
"""
from __future__ import annotations

import logging
from dataclasses import dataclass
from typing import List, Tuple

from .fetcher import OptionQuote
from ..logging_utils import get_logger

logger = get_logger()


@dataclass
class CleaningStats:
    raw_count: int = 0
    after_liquidity: int = 0
    after_moneyness: int = 0
    after_calendar_arbitrage: int = 0
    after_butterfly_arbitrage: int = 0
    final_count: int = 0

    def __str__(self) -> str:
        return (
            f"raw={self.raw_count} → "
            f"liquidity={self.after_liquidity} → "
            f"moneyness={self.after_moneyness} → "
            f"calendar_arb={self.after_calendar_arbitrage} → "
            f"butterfly_arb={self.after_butterfly_arbitrage}"
        )


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _is_call_otm(q: OptionQuote, spot: float) -> bool:
    """True when the option is OTM or ATM (preferred for surface fitting)."""
    if q.option_type == "call":
        return q.strike >= spot
    return q.strike <= spot


def _liquidity_ok(
    q: OptionQuote,
    min_oi: int,
    min_bid: float,
    max_spread_ratio: float,
) -> str | None:
    """Return None if OK, or a short reason string if the quote fails."""
    if q.open_interest < min_oi:
        return "low_oi"
    if q.implied_volatility is None or q.implied_volatility <= 0:
        return "no_iv"
    if q.mid <= 0:
        return "mid_le0"
    if q.bid < min_bid:
        return "low_bid"
    spread = q.ask - q.bid
    if q.mid > 0 and spread / q.mid > max_spread_ratio:
        return "wide_spread"
    return None


def _moneyness_ok(q: OptionQuote, spot: float, lo: float, hi: float) -> bool:
    m = q.strike / spot
    return lo <= m <= hi


# ---------------------------------------------------------------------------
# Calendar arbitrage: total variance w = IV^2 * T must be non-decreasing in T
# for each fixed (strike, option_type) pair.
# ---------------------------------------------------------------------------

def _remove_calendar_arbitrage(quotes: List[OptionQuote]) -> List[OptionQuote]:
    """
    Per (option_type, strike) series: drop any quote where w = IV²·T is
    smaller than an earlier (shorter-dated) quote's w.  This ensures the
    total variance surface is monotone in maturity.
    """
    from collections import defaultdict
    # group by (type, strike rounded to nearest 0.50 to handle minor float diffs)
    buckets: dict = defaultdict(list)
    for q in quotes:
        key = (q.option_type, round(q.strike * 2) / 2)
        buckets[key].append(q)

    clean: List[OptionQuote] = []
    for series in buckets.values():
        series.sort(key=lambda q: q.ttm)
        max_w = -1.0
        for q in series:
            w = q.implied_volatility ** 2 * q.ttm   # type: ignore[operator]
            if w >= max_w:
                max_w = w
                clean.append(q)
            # else: calendar arbitrage — drop silently

    return clean


# ---------------------------------------------------------------------------
# Butterfly arbitrage (discrete): calls must be convex in K per expiry.
# A simple check: for consecutive strikes K1 < K2 < K3:
#   C(K1) - 2*C(K2) + C(K3) >= 0
# We drop quotes that cause violations until no violation remains.
# We only do this for calls (put–call parity gives the same surface).
# ---------------------------------------------------------------------------

def _remove_butterfly_arbitrage(quotes: List[OptionQuote]) -> List[OptionQuote]:
    """Remove butterfly-arbitrage violations per expiry slice (calls only)."""
    calls = [q for q in quotes if q.option_type == "call"]
    puts  = [q for q in quotes if q.option_type == "put"]

    from collections import defaultdict
    by_expiry: dict = defaultdict(list)
    for q in calls:
        by_expiry[q.expiry].append(q)

    clean_calls: List[OptionQuote] = []
    for exp_quotes in by_expiry.values():
        exp_quotes.sort(key=lambda q: q.strike)
        # Iterative removal: keep going until no violation found
        changed = True
        while changed and len(exp_quotes) >= 3:
            changed = False
            keep = [True] * len(exp_quotes)
            for i in range(1, len(exp_quotes) - 1):
                if not (keep[i - 1] and keep[i] and keep[i + 1]):
                    continue
                c0 = exp_quotes[i - 1].mid
                c1 = exp_quotes[i].mid
                c2 = exp_quotes[i + 1].mid
                if c0 - 2 * c1 + c2 < 0:
                    keep[i] = False
                    changed = True
            exp_quotes = [q for q, ok in zip(exp_quotes, keep) if ok]
        clean_calls.extend(exp_quotes)

    return clean_calls + puts


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

def clean_chain(
    quotes: List[OptionQuote],
    spot: float,
    min_open_interest: int = 10,
    min_bid: float = 0.05,
    max_spread_ratio: float = 0.50,
    min_moneyness: float = 0.70,
    max_moneyness: float = 1.40,
) -> Tuple[List[OptionQuote], CleaningStats]:
    """
    Apply a multi-stage cleaning pipeline to raw option quotes.

    Parameters
    ----------
    quotes : list[OptionQuote]
        Raw output of :func:`fetcher.fetch_option_chain`.
    spot : float
        Current underlier spot price.
    min_open_interest : int
        Minimum open interest (default 10).
    min_bid : float
        Minimum bid price accepted (default 0.05).
    max_spread_ratio : float
        Maximum (ask-bid)/mid ratio (default 0.50).
    min_moneyness, max_moneyness : float
        Strike/spot moneyness bounds — keeps strikes in [0.70, 1.40] by default.

    Returns
    -------
    (clean_quotes, stats) : (list[OptionQuote], CleaningStats)
    """
    stats = CleaningStats(raw_count=len(quotes))
    logger.info(
        "cleaner: START | raw=%d quotes | spot=%.2f | OI>=%d bid>=%.2f spread<=%.0f%% moneyness=[%.2f,%.2f]",
        len(quotes), spot, min_open_interest, min_bid, max_spread_ratio * 100,
        min_moneyness, max_moneyness,
    )

    # --- 1. Liquidity ---
    from collections import Counter as _Counter
    liq = []
    reject_reasons: dict[str, int] = {}
    for q in quotes:
        reason = _liquidity_ok(q, min_open_interest, min_bid, max_spread_ratio)
        if reason is None:
            liq.append(q)
        else:
            reject_reasons[reason] = reject_reasons.get(reason, 0) + 1
    stats.after_liquidity = len(liq)
    logger.info(
        "cleaner: LIQUIDITY | kept %d / %d | rejected breakdown: %s",
        len(liq), len(quotes), reject_reasons,
    )

    # --- 2. Moneyness ---
    mon = [q for q in liq if _moneyness_ok(q, spot, min_moneyness, max_moneyness)]
    stats.after_moneyness = len(mon)
    n_mon_dropped = len(liq) - len(mon)
    if n_mon_dropped > 0:
        sample = [q for q in liq if not _moneyness_ok(q, spot, min_moneyness, max_moneyness)][:5]
        sample_moneyness = [(round(q.strike / spot, 3), q.strike, q.option_type) for q in sample]
        logger.info(
            "cleaner: MONEYNESS | kept %d / %d (dropped %d) | sample dropped (m, K, type): %s",
            len(mon), len(liq), n_mon_dropped, sample_moneyness,
        )
    else:
        logger.info("cleaner: MONEYNESS | kept %d / %d (none dropped)", len(mon), len(liq))

    # --- 3. Calendar arbitrage ---
    cal = _remove_calendar_arbitrage(mon)
    stats.after_calendar_arbitrage = len(cal)
    logger.info(
        "cleaner: CALENDAR ARB | kept %d / %d (dropped %d)",
        len(cal), len(mon), len(mon) - len(cal),
    )

    # --- 4. Butterfly arbitrage (calls) ---
    but = _remove_butterfly_arbitrage(cal)
    stats.after_butterfly_arbitrage = len(but)
    logger.info(
        "cleaner: BUTTERFLY ARB | kept %d / %d (dropped %d)",
        len(but), len(cal), len(cal) - len(but),
    )

    stats.final_count = len(but)
    logger.info("cleaner: DONE | %s", stats)

    return but, stats
