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


def check_script(script: str, valuation_date: str, day_count: str) -> CheckResult:
    try:
        parsed = qm.validate_script(script, valuation_date, day_count)
    except (RuntimeError, ValueError) as exc:
        return CheckResult(False, error=str(exc))
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
        return CheckResult(False, error=str(exc))
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
