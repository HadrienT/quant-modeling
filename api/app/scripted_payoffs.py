"""Path-dependent products as payoff scripts (blueprint/wp/16-scripting.md),
so that a product that has started to live is priced from where it is.

An Asian option's average already includes the fixings that happened; a
lookback's extremum is already partly known; a barrier may already be
breached. The catalog's own pricers start every path from today, as if the
product had just been struck. Here each product is written as a script on
its contract's real calendar: every observation date, from the contract's
start to its expiry, on the exchange calendar of its currency. Savine's
engine (ScriptedProduct, lot 16e) replays the dates on or before the
valuation date once, against the stored closes, and simulates only the
future ones. A knocked-out barrier is then worth its rebate, and an Asian
carries its partial average.

Observation schedule: one fixing per exchange business day, at the close —
the usual terms of listed-equity Asians, lookbacks and barriers (daily-close
monitoring, not continuous). The dates are resolved by the engine's own
schedule generator, so the script, its fixings and the simulation agree on
every date.
"""

from __future__ import annotations

import math
from dataclasses import dataclass
from datetime import date, timedelta
from typing import Dict, List, Tuple

import quantmodeling as qm

#: Exchange calendar of the scripting engine, per currency (NONE = weekdays).
CALENDARS = {"USD": "US", "EUR": "TARGET", "GBP": "UK"}


@dataclass(frozen=True)
class ScriptedTerms:
    """A product's script and its observation dates (the last is expiry)."""

    script: str
    dates: Tuple[date, ...]


def observation_dates(start: date, expiry: date, currency: str) -> List[date]:
    """Every business day from start to expiry, both included, as the
    engine's schedule() resolves them (resolved before the start date, so
    that no date is historical yet)."""
    cal = CALENDARS.get(currency, "NONE")
    probe = f"schedule({start.isoformat()}, {expiry.isoformat()}, 1D, {cal}, F)\n    x = spot()\n"
    resolved = qm.validate_script(probe, (start - timedelta(days=1)).isoformat())
    days = sorted({date.fromisoformat(e["date"]) for e in resolved["events"]})
    # A holiday on the expiry rolls forward past it; the payment is on expiry.
    days = [d for d in days if d < expiry]
    return days + [expiry]


def _dates_line(dates: List[date]) -> str:
    return "  ".join(d.isoformat() for d in dates)


def _events(first: str, middle: str, last: str, dates: List[date]) -> str:
    """Three events: the first date, every date in between (literal dates,
    never moved), and the expiry."""
    out = f"{dates[0].isoformat()}\n{first}\n"
    if len(dates) > 2:
        out += f"\n{_dates_line(dates[1:-1])}\n{middle}\n"
    out += f"\n{dates[-1].isoformat()}\n{last}\n"
    return out


def _ind(*lines: str) -> str:
    return "\n".join("    " + ln for ln in lines)


def asian(p: dict, dates: List[date]) -> str:
    """Average-price Asian on every fixing: arithmetic, or geometric (mean
    of the logs)."""
    k, call = float(p["strike"]), bool(p["is_call"])
    geometric = p.get("average_type") == "geometric"
    obs = "log(spot())" if geometric else "spot()"
    avg = "exp(acc / n)" if geometric else "acc / n"
    payoff = f"max({avg} - {k}, 0)" if call else f"max({k} - {avg}, 0)"
    return _events(
        _ind(f"acc = {obs}", "n = 1"),
        _ind(f"acc = acc + {obs}", "n = n + 1"),
        _ind(f"acc = acc + {obs}", "n = n + 1", f"pays {payoff}"),
        dates,
    )


def lookback(p: dict, dates: List[date]) -> str:
    """Fixed strike: the extremum against the strike. Floating strike: the
    final close against the extremum (a call on the minimum, a put on the
    maximum)."""
    k, call = float(p["strike"]), bool(p["is_call"])
    floating = p.get("style") == "floating-strike"
    upd = _ind("hi = max(hi, spot())", "lo = min(lo, spot())")
    if floating:
        payoff = "spot() - lo" if call else "hi - spot()"
    elif p.get("extremum", "maximum") == "maximum":
        payoff = f"max(hi - {k}, 0)" if call else f"max({k} - hi, 0)"
    else:
        payoff = f"max(lo - {k}, 0)" if call else f"max({k} - lo, 0)"
    return _events(
        _ind("hi = spot()", "lo = spot()"),
        upd,
        upd + "\n" + _ind(f"pays {payoff}"),
        dates,
    )


def barrier(p: dict, dates: List[date]) -> str:
    """Knock-out: the vanilla dies the first close beyond the barrier, the
    rebate paid then. Knock-in: the vanilla exists only if a close crossed
    it; otherwise the rebate is paid at expiry."""
    k, call = float(p["strike"]), bool(p["is_call"])
    level, kind = float(p["barrier_level"]), str(p["barrier_kind"])
    rebate = float(p.get("rebate", 0.0) or 0.0)
    cross = f"spot() >= {level}" if kind.startswith("up") else f"spot() <= {level}"
    vanilla = f"max(spot() - {k}, 0)" if call else f"max({k} - spot(), 0)"
    if kind.endswith("out"):
        # One statement per line inside a block (the parser's rule).
        hit = [f"if alive = 1 and {cross} then", "    alive = 0"]
        hit += [f"    pays {rebate}", "endIf"] if rebate else ["endIf"]
        return _events(
            _ind("alive = 1", *hit),
            _ind(*hit),
            _ind(*hit, f"if alive = 1 then pays {vanilla} endIf"),
            dates,
        )
    seen = f"if {cross} then hit = 1 endIf"
    final = f"if hit = 1 then pays {vanilla}"
    final += f" else pays {rebate} endIf" if rebate else " endIf"
    return _events(_ind("hit = 0", seen), _ind(seen), _ind(seen, final), dates)


BUILDERS = {"asian": asian, "lookback": lookback, "barrier": barrier}


def terms(
    product: str, params: dict, start: date, expiry: date, currency: str
) -> ScriptedTerms:
    dates = observation_dates(start, expiry, currency)
    return ScriptedTerms(BUILDERS[product](params, dates), tuple(dates))


def path_state(
    product: str, params: dict, fixings: List[Tuple[date, float]]
) -> Dict[str, float]:
    """What the fixings already done say, for display (the engine computes
    its own from the same fixings)."""
    if not fixings:
        return {}
    closes = [c for _, c in fixings]
    out: Dict[str, float] = {"fixings done": float(len(closes))}
    if product == "asian":
        if params.get("average_type") == "geometric":
            out["average so far"] = math.exp(sum(map(math.log, closes)) / len(closes))
        else:
            out["average so far"] = sum(closes) / len(closes)
    elif product == "lookback":
        out["maximum so far"] = max(closes)
        out["minimum so far"] = min(closes)
    elif product == "barrier":
        level, kind = float(params["barrier_level"]), str(params["barrier_kind"])
        crossed = any(
            c >= level if kind.startswith("up") else c <= level for c in closes
        )
        out["barrier crossed"] = 1.0 if crossed else 0.0
    return out


__all__ = [
    "BUILDERS",
    "CALENDARS",
    "ScriptedTerms",
    "barrier",
    "lookback",
    "asian",
    "observation_dates",
    "path_state",
    "terms",
]
