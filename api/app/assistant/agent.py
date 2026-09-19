"""The scripting assistant's loop: draft -> check with the real parser -> repair.

The model writes a script in a ```qms fence. The server runs that script
through the same C++ parser and timeline resolution as the editor's Validate
button, plus a 1 000-path smoke pricing to catch what the parser cannot see
(NaN from a division by zero or a log of a negative number).
A rejected script goes back to the model together with the exact error,
up to MAX_REPAIRS times; the user only sees the final answer. So a script
offered to the user has, by construction, passed the parser — the model is
never trusted on syntax.
"""

from __future__ import annotations

import math
import re
from dataclasses import dataclass
from datetime import date, timedelta
from typing import Callable, Iterator, Sequence

import quantmodeling as qm

from .llm import ChatModel, LLMError, Message
from .prompt import build_system_prompt

MAX_REPAIRS = 2

_FENCE = re.compile(r"```(?:qms)?[ \t]*\n(.*?)```", re.DOTALL)
_SMOKE = dict(spot=100.0, rate=0.03, dividend=0.0, vol=0.2, n_paths=1000, seed=1)


@dataclass(frozen=True)
class CheckResult:
    ok: bool
    error: str | None = None
    events: list[dict] | None = None
    variables: list[str] | None = None


Checker = Callable[[str, str, str], CheckResult]


def extract_script(text: str) -> str | None:
    blocks = _FENCE.findall(text)
    if not blocks:
        return None
    # Last block wins: a reply may quote an old snippet before the answer.
    script = blocks[-1].strip("\n")
    return script if script.strip() else None


_ISO = r"\d{4}-\d{2}-\d{2}"
_SCHEDULE_END = re.compile(rf"schedule\(\s*{_ISO}\s*,\s*({_ISO})")
_SPAN_TO_IGNORE = re.compile(rf"#[^\n]*|schedule\([^)]*\)|df\([^)]*\)")


def _schedule_overflow(script: str, event_dates: list[str]) -> str | None:
    """schedule() moves a date that lands on a weekend, a literal event date
    is never moved. When a schedule's raw END is also written as its own event,
    the adjusted last observation can fall AFTER that event — so a payment
    written there silently misses the last observation. The parser cannot
    see it (both dates are valid); a model keeps writing it anyway."""
    literal = set(re.findall(_ISO, _SPAN_TO_IGNORE.sub("", script)))
    resolved = [date.fromisoformat(d) for d in event_dates]
    for end in _SCHEDULE_END.findall(script):
        if end not in literal:
            continue
        end_d = date.fromisoformat(end)
        moved = [
            d
            for d in resolved
            if end_d < d <= end_d + timedelta(days=7) and d.isoformat() not in literal
        ]
        if moved:
            return (
                f"schedule(...) ends on {end}, but its convention moves the last "
                f"date to {moved[0].isoformat()}, AFTER the event you wrote on "
                f"{end}: that event runs first and the last observation comes too "
                "late (it is not part of the payment). End the schedule strictly "
                "before that date and put the last observation, and the payment, "
                "in the event written on a business day (or use convention U)."
            )
    return None


def _explain(error: str) -> str:
    if "historical fixing" in error:
        return (
            f"{error}\nAn event on or before the valuation date needs a "
            "historical fixing, and this playground has no way to enter one: "
            "every event must be strictly after the valuation date."
        )
    return error


def check_script(script: str, valuation_date: str, day_count: str) -> CheckResult:
    try:
        parsed = qm.validate_script(script, valuation_date, day_count)
    except (RuntimeError, ValueError) as exc:
        return CheckResult(False, error=_explain(str(exc)))
    overflow = _schedule_overflow(script, [e["date"] for e in parsed["events"]])
    if overflow:
        return CheckResult(False, error=overflow)
    try:
        priced = qm.price_script(
            script,
            _SMOKE["spot"],
            _SMOKE["rate"],
            _SMOKE["dividend"],
            _SMOKE["vol"],
            valuation_date,
            day_count,
            False,
            0.01,
            _SMOKE["n_paths"],
            _SMOKE["seed"],
            "pseudo",
            "none",
        )
    except (RuntimeError, ValueError) as exc:
        return CheckResult(False, error=_explain(str(exc)))
    if not math.isfinite(priced["npv"]):
        return CheckResult(
            False,
            error=(
                "The script parses but its simulated value is NaN or infinite "
                "(spot=100, vol=20%): look for a division by zero or a log/sqrt "
                "of a negative number, or a variable used before it is assigned."
            ),
        )
    return CheckResult(
        True, events=parsed["events"], variables=list(parsed["variables"])
    )


def _repair_message(error: str) -> Message:
    return {
        "role": "user",
        "content": (
            "The parser rejected the script you just wrote:\n\n"
            f"{error}\n\n"
            "Fix it and answer again with the complete corrected script in a "
            "single ```qms block, followed by a one-sentence note on what "
            "you changed. Do not alter the product terms the user gave (dates, "
            "levels, notional) just to make it parse: if the error comes from "
            "the user's own terms, do not write a script, explain the problem "
            "instead."
        ),
    }


def run_assistant(
    *,
    model: ChatModel,
    history: Sequence[Message],
    valuation_date: str,
    day_count: str,
    current_script: str | None,
    last_error: str | None,
    checker: Checker = check_script,
) -> Iterator[dict]:
    """Yield wire events (see schemas.py): delta / retry / script / error / done."""
    messages: list[Message] = [
        {
            "role": "system",
            "content": build_system_prompt(
                valuation_date=valuation_date,
                current_script=current_script,
                last_error=last_error,
            ),
        },
        *history,
    ]
    try:
        for attempt in range(MAX_REPAIRS + 1):
            reply = ""
            for delta in model.stream(messages):
                reply += delta
                yield {"type": "delta", "text": delta}

            script = extract_script(reply)
            if script is None:
                break  # a question or an explanation: nothing to check

            result = checker(script, valuation_date, day_count)
            if result.ok:
                yield {
                    "type": "script",
                    "script": script,
                    "valid": True,
                    "error": None,
                    "events": result.events,
                    "variables": result.variables,
                }
                break
            if attempt == MAX_REPAIRS:
                # Give up honestly: show the last draft, flagged as invalid.
                yield {
                    "type": "script",
                    "script": script,
                    "valid": False,
                    "error": result.error,
                    "events": None,
                    "variables": None,
                }
                break
            yield {"type": "retry", "attempt": attempt + 1, "error": result.error}
            messages += [
                {"role": "assistant", "content": reply},
                _repair_message(result.error or "unknown error"),
            ]
    except LLMError as exc:
        yield {"type": "error", "message": str(exc)}
    yield {"type": "done"}
