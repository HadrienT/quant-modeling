"""Scripting assistant: prompt fidelity, the draft/check/repair loop, the route.

Runs against the real C++ parser (the `quantmodeling` wheel) with a scripted
fake standing in for the LLM — nothing here needs llama-server.
Run from the repo root:  pytest api/tests
"""

from __future__ import annotations

import asyncio
import json
import os

os.environ.setdefault("JWT_SECRET", "test-only-secret-not-for-deployment")

import pytest
import quantmodeling as qm
from fastapi import HTTPException

from api.app.assistant import agent
from api.app.assistant.agent import (
    MAX_REPAIRS,
    check_script,
    extract_script,
    run_assistant,
)
from api.app.assistant.llm import LLMError
from api.app.assistant.prompt import EXAMPLES, build_system_prompt
from api.app.assistant.schemas import ScriptingChatEvent, ScriptingChatRequest
from api.app.auth import require_user
from api.app.main import _validation_exception_handler
from api.app.routers import assistant as route
from fastapi.exceptions import RequestValidationError
from pydantic import ValidationError

VALUATION = "2026-09-19"
GOOD = "2027-09-10\n    pays max(spot() - 100, 0)"
BAD = "2027-09-10\n    pays max(spot - 100, 0)"  # spot without parentheses


class FakeModel:
    """Replies with the given texts in order, one per `stream` call, in
    small chunks; remembers the messages it was sent."""

    def __init__(self, *replies: str | Exception):
        self._replies = list(replies)
        self.calls: list[list[dict]] = []

    def stream(self, messages):
        self.calls.append([dict(m) for m in messages])
        reply = self._replies.pop(0)
        if isinstance(reply, Exception):
            raise reply
        for i in range(0, len(reply), 7):
            yield reply[i : i + 7]


def fenced(script: str, note: str = "Done.") -> str:
    return f"Here you go:\n```qms\n{script}\n```\n{note}"


def run(model, **kw):
    return list(
        run_assistant(
            model=model,
            history=[{"role": "user", "content": "a european call"}],
            valuation_date=VALUATION,
            day_count="ACT/365F",
            current_script=kw.get("current_script"),
            last_error=kw.get("last_error"),
        )
    )


def types(events):
    return [e["type"] for e in events]


# --- the prompt must not teach the model a dead idiom -------------------------


@pytest.mark.parametrize("title", list(EXAMPLES))
def test_every_prompt_example_passes_the_real_parser(title):
    result = check_script(EXAMPLES[title], VALUATION, "ACT/365F")
    assert result.ok, result.error


def test_asian_schedule_example_observes_every_month_up_to_the_payment():
    """The prompt teaches this pattern because schedule() adjusts dates and a
    literal date does not: the last fixing must be in the maturity event, and
    every schedule date must fall before it."""
    title = next(t for t in EXAMPLES if t.startswith("Arithmetic Asian call, monthly"))
    events = qm.validate_script(EXAMPLES[title], VALUATION, "ACT/365F")["events"]
    dates = [e["date"] for e in events]
    assert dates == sorted(dates) and len(dates) == len(set(dates))
    assert dates[0] == "2026-10-19" and dates[-1] == "2027-09-20"
    assert len(dates) == 12  # initial + 10 monthly fixings + maturity


ASIAN_TRAP = (
    "2026-10-19\n    acc = spot()\n    n = 1\n\n"
    "schedule(2026-11-19, 2027-09-19, 1M, TARGET, MF)\n"
    "    acc = acc + spot()\n    n = n + 1\n\n"
    "2027-09-19\n    acc = acc + spot()\n    n = n + 1\n"
    "    pays max(acc / n - 100, 0)\n"
)  # 2027-09-19 is a Sunday: the schedule's last date becomes Monday the 20th


def test_checker_catches_a_schedule_whose_adjusted_end_overtakes_its_own_event():
    r = check_script(ASIAN_TRAP, VALUATION, "ACT/365F")
    assert not r.ok and "2027-09-20" in r.error and "AFTER" in r.error


def test_checker_leaves_a_schedule_that_ends_before_the_maturity_event():
    ok = ASIAN_TRAP.replace("2027-09-19, 1M", "2027-08-19, 1M").replace(
        "\n2027-09-19\n", "\n2027-09-20\n"
    )
    assert check_script(ok, VALUATION, "ACT/365F").ok


def test_prompt_carries_editor_context():
    prompt = build_system_prompt(
        valuation_date=VALUATION,
        current_script="2027-01-01\n    pays 1",
        last_error="line 2, col 5: boom",
    )
    assert "pays 1" in prompt and "boom" in prompt and VALUATION in prompt
    bare = build_system_prompt(
        valuation_date=VALUATION, current_script="  ", last_error=None
    )
    assert "CURRENT SCRIPT" not in bare


# --- extraction ---------------------------------------------------------------


def test_extract_takes_the_last_block_and_ignores_empty_ones():
    text = f"old:\n```qms\n{BAD}\n```\nnew:\n```qms\n{GOOD}\n```"
    assert extract_script(text) == GOOD
    assert extract_script("no code here") is None
    assert extract_script("```qms\n\n```") is None
    assert extract_script(f"```\n{GOOD}\n```") == GOOD  # untagged fence


# --- the checker --------------------------------------------------------------


def test_checker_accepts_a_valid_script_and_reports_its_timeline():
    r = check_script(GOOD, VALUATION, "ACT/365F")
    assert r.ok and r.events and r.events[0]["date"] == "2027-09-10"


def test_checker_returns_the_parsers_pointed_error():
    r = check_script(BAD, VALUATION, "ACT/365F")
    assert not r.ok and "line 2" in r.error


def test_checker_rejects_a_past_event():
    r = check_script("2020-01-01\n    pays 1", VALUATION, "ACT/365F")
    assert not r.ok and "strictly after the valuation date" in r.error


@pytest.mark.parametrize("payoff", ["1 / 0", "log(0 - 1)"])
def test_checker_rejects_a_script_that_simulates_to_nan(payoff):
    r = check_script(f"2027-09-10\n    pays {payoff}", VALUATION, "ACT/365F")
    assert not r.ok and "NaN" in r.error


# --- the loop -----------------------------------------------------------------


def test_valid_first_draft_is_streamed_then_offered_once():
    model = FakeModel(fenced(GOOD))
    events = run(model)
    assert types(events)[-1] == "done"
    assert types(events).count("script") == 1 and "retry" not in types(events)
    script = next(e for e in events if e["type"] == "script")
    assert script["valid"] and script["script"] == GOOD
    streamed = "".join(e["text"] for e in events if e["type"] == "delta")
    assert streamed == fenced(GOOD)


def test_rejected_draft_is_repaired_with_the_parsers_error_in_context():
    model = FakeModel(fenced(BAD), fenced(GOOD, "Added the parentheses."))
    events = run(model)
    assert types(events).count("retry") == 1
    script = next(e for e in events if e["type"] == "script")
    assert script["valid"] and script["script"] == GOOD
    repair_call = model.calls[1]
    assert repair_call[-2] == {"role": "assistant", "content": fenced(BAD)}
    assert "expected '('" in repair_call[-1]["content"]


def test_gives_up_after_max_repairs_and_flags_the_script_invalid():
    model = FakeModel(*[fenced(BAD)] * (MAX_REPAIRS + 1))
    events = run(model)
    assert types(events).count("retry") == MAX_REPAIRS
    script = next(e for e in events if e["type"] == "script")
    assert script["valid"] is False and script["error"]
    assert len(model.calls) == MAX_REPAIRS + 1


def test_a_clarifying_question_has_no_script_and_no_check(monkeypatch):
    def boom(*_a):
        raise AssertionError("checker must not run without a script")

    monkeypatch.setattr(agent, "check_script", boom)
    events = run(FakeModel("What is the strike and the maturity?"))
    assert types(events)[-1] == "done" and "script" not in types(events)


def test_model_failure_is_reported_and_the_stream_still_ends():
    events = run(FakeModel(LLMError("model server unreachable")))
    assert types(events) == ["error", "done"]


def test_every_event_fits_the_documented_wire_schema():
    events = run(FakeModel(fenced(BAD), fenced(GOOD)))
    for e in events:
        ScriptingChatEvent.model_validate(e)


# --- the route ----------------------------------------------------------------


def _request(**kw):
    return ScriptingChatRequest(messages=[{"role": "user", "content": "a call"}], **kw)


def _drain(response) -> list[dict]:
    async def collect():
        chunks = [c async for c in response.body_iterator]
        return chunks

    raw = "".join(
        c.decode() if isinstance(c, bytes) else c for c in asyncio.run(collect())
    )
    return [json.loads(b[len("data: ") :]) for b in raw.split("\n\n") if b]


def test_request_must_end_with_a_user_message():
    with pytest.raises(ValueError):
        ScriptingChatRequest(messages=[{"role": "assistant", "content": "hi"}])


def test_invalid_request_is_a_422_not_a_500():
    """A validator that raises ValueError puts the exception in `ctx.error`;
    the app-wide handler used to choke on serialising it."""
    try:
        ScriptingChatRequest(messages=[{"role": "assistant", "content": "hi"}])
    except ValidationError as exc:
        wrapped = RequestValidationError(exc.errors())
    resp = asyncio.run(_validation_exception_handler(None, wrapped))
    assert resp.status_code == 422
    body = json.loads(resp.body)
    assert "last message must come from the user" in json.dumps(body["detail"])


def test_route_streams_sse_and_frees_its_slot(monkeypatch):
    monkeypatch.setattr(
        route.LlamaServerClient, "from_env", lambda: FakeModel(fenced(GOOD))
    )
    resp = route.scripting_chat_endpoint(_request(valuation_date=VALUATION))
    assert resp.media_type == "text/event-stream"
    events = _drain(resp)
    assert types(events)[-1] == "done"
    assert any(e["type"] == "script" and e["valid"] for e in events)
    # Both slots are available again: acquiring them all must succeed.
    for _ in range(route._MAX_CONCURRENT_CHATS):
        assert route._slots.acquire(blocking=False)
    for _ in range(route._MAX_CONCURRENT_CHATS):
        route._slots.release()


def test_route_answers_429_when_every_slot_is_busy():
    taken = 0
    try:
        while route._slots.acquire(blocking=False):
            taken += 1
        with pytest.raises(HTTPException) as exc:
            route.scripting_chat_endpoint(_request())
        assert exc.value.status_code == 429
    finally:
        for _ in range(taken):
            route._slots.release()


def test_slot_is_released_once_even_if_the_stream_never_starts(monkeypatch):
    monkeypatch.setattr(
        route.LlamaServerClient, "from_env", lambda: FakeModel(fenced(GOOD))
    )
    resp = route.scripting_chat_endpoint(_request(valuation_date=VALUATION))
    # Client vanished before the first byte: only the background task runs,
    # and running it twice (as a stream `finally` would) must not raise.
    asyncio.run(resp.background())
    asyncio.run(resp.background())
    for _ in range(route._MAX_CONCURRENT_CHATS):
        assert route._slots.acquire(blocking=False)
    for _ in range(route._MAX_CONCURRENT_CHATS):
        route._slots.release()


def test_route_requires_a_signed_in_user():
    """The prod app is public through the tunnel and this route spends the
    owner's GPU: it must sit behind the same JWT dependency as portfolios."""
    (r,) = [r for r in route.router.routes if r.path.endswith("/scripting/chat")]
    assert require_user in [d.call for d in r.dependant.dependencies]
