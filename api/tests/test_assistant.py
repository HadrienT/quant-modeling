"""Scripting assistant: prompt fidelity, the draft/check/repair loop, the route.

Runs against the real C++ parser (the `quantmodeling` wheel) with a scripted
fake standing in for the LLM — nothing here calls the Anthropic API.
Run from the repo root:  pytest api/tests
"""

from __future__ import annotations

import asyncio
from datetime import date
import json
import os

os.environ.setdefault("JWT_SECRET", "test-only-secret-not-for-deployment")

import pytest
import quantmodeling as qm
from fastapi import HTTPException
from fastapi.testclient import TestClient

from api.app.assistant import agent, quota
from api.app.assistant.agent import (
    MAX_REPAIRS,
    check_script,
    extract_script,
    run_assistant,
)
from api.app.assistant.llm import LLMError, LLMNotConfigured
from api.app.assistant.prompt import EXAMPLES, build_system_prompt
from api.app.assistant.schemas import ScriptingChatEvent, ScriptingChatRequest
from api.app.auth import optional_user
from api.app.main import _validation_exception_handler, app
from api.app.routers import assistant as route
from fastapi.exceptions import RequestValidationError
from pydantic import ValidationError

VALUATION = "2026-09-19"
GOOD = "2027-09-10\n    pays max(spot() - 100, 0)"
BAD = "2027-09-10\n    pays max(spot - 100, 0)"  # spot without parentheses


class FakeModel:
    """Replies with the given texts in order, one per `stream` call, in
    small chunks; remembers the messages it was sent."""

    model = "fake"

    def __init__(self, *replies: str | Exception):
        self._replies = list(replies)
        self.calls: list[list[dict]] = []
        self.systems: list[str] = []

    def stream(self, system, messages):
        self.systems.append(system)
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
    monkeypatch.setattr(route.ClaudeClient, "from_env", lambda: FakeModel(fenced(GOOD)))
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
    monkeypatch.setattr(route.ClaudeClient, "from_env", lambda: FakeModel(fenced(GOOD)))
    resp = route.scripting_chat_endpoint(_request(valuation_date=VALUATION))
    # Client vanished before the first byte: only the background task runs,
    # and running it twice (as a stream `finally` would) must not raise.
    asyncio.run(resp.background())
    asyncio.run(resp.background())
    for _ in range(route._MAX_CONCURRENT_CHATS):
        assert route._slots.acquire(blocking=False)
    for _ in range(route._MAX_CONCURRENT_CHATS):
        route._slots.release()


# ── Visitors who are not signed in: a few questions a day (issue #175) ───────


@pytest.fixture
def anonymous(monkeypatch):
    """A fresh allowance of three questions, and a model that always answers."""
    monkeypatch.setattr(route.ClaudeClient, "from_env", lambda: FakeModel(fenced(GOOD)))
    allowance = quota.AnonymousQuota(per_visitor=3, total=100)
    monkeypatch.setattr(route, "_anonymous_quota", allowance)
    return allowance


def _ask(user=None):
    return route.scripting_chat_endpoint(_request(valuation_date=VALUATION), user=user)


def test_route_is_open_to_visitors_but_reads_the_session():
    (r,) = [r for r in route.router.routes if r.path.endswith("/scripting/chat")]
    assert optional_user in [d.call for d in r.dependant.dependencies]


def test_visitor_is_told_what_is_left_then_asked_to_sign_in(anonymous):
    left = [_drain(_ask())[0] for _ in range(3)]
    assert left == [{"type": "quota", "remaining": n} for n in (2, 1, 0)]
    with pytest.raises(HTTPException) as exc:
        _ask()
    assert exc.value.status_code == 429
    assert exc.value.code == route.ANONYMOUS_QUOTA_CODE
    # The refusal held no slot back.
    assert route._anonymous_slots.acquire(blocking=False)
    route._anonymous_slots.release()


def test_exhausted_quota_reaches_the_client_under_its_own_code(anonymous):
    client = TestClient(app)
    body = {"messages": [{"role": "user", "content": "a call"}]}
    for _ in range(3):
        assert (
            client.post("/api/assistant/scripting/chat", json=body).status_code == 200
        )
    r = client.post("/api/assistant/scripting/chat", json=body)
    assert r.status_code == 429
    assert r.json()["code"] == "anonymous_quota_exhausted"
    assert "Sign in" in r.json()["message"]


def test_signed_in_user_is_not_counted(anonymous):
    for _ in range(5):
        assert types(_drain(_ask(user="alice")))[0] != "quota"
    assert _drain(_ask())[0] == {"type": "quota", "remaining": 2}


def test_each_visitor_has_their_own_allowance_under_a_shared_ceiling():
    q = quota.AnonymousQuota(per_visitor=2, total=3)
    assert [q.consume("a"), q.consume("a"), q.consume("b")] == [1, 0, 1]
    with pytest.raises(quota.QuotaExhausted) as exc:
        q.consume("a")
    assert exc.value.scope == "visitor"
    with pytest.raises(quota.QuotaExhausted) as exc:
        q.consume("b")
    assert exc.value.scope == "global"


def test_allowance_comes_back_the_next_day():
    day = [date(2026, 10, 13)]
    q = quota.AnonymousQuota(per_visitor=1, total=10, today=lambda: day[0])
    assert q.consume("a") == 0
    with pytest.raises(quota.QuotaExhausted):
        q.consume("a")
    day[0] = date(2026, 10, 14)
    assert q.consume("a") == 0


def test_busy_assistant_does_not_cost_a_visitor_a_question(anonymous):
    assert route._anonymous_slots.acquire(blocking=False)
    try:
        with pytest.raises(HTTPException) as exc:
            _ask()
        assert getattr(exc.value, "code", None) is None
        # The slot kept for signed-in users is still theirs.
        assert types(_drain(_ask(user="alice")))[-1] == "done"
    finally:
        route._anonymous_slots.release()
    assert _drain(_ask())[0] == {"type": "quota", "remaining": 2}


# ── Status: is the model server up, and with which model ─────────────────────


def _status(monkeypatch, probe, key="sk-test"):
    """The status the route gives when the API probe returns (None) or raises."""

    def fake_probe(self):
        if self.api_key is None:
            raise LLMNotConfigured(
                "The assistant is not configured on this server (no API key)."
            )
        if isinstance(probe, Exception):
            raise probe

    monkeypatch.setattr(route.ClaudeClient, "probe", fake_probe)
    if key is None:
        monkeypatch.delenv("ANTHROPIC_API_KEY", raising=False)
    else:
        monkeypatch.setenv("ANTHROPIC_API_KEY", key)
    monkeypatch.setenv("QM_LLM_MODEL", "claude-haiku-5-5")
    monkeypatch.setattr(route, "_STATUS_CACHE", route.TTLCache(1, 15))
    r = TestClient(app).get("/api/assistant/status")
    assert r.status_code == 200, r.text
    return r.json()


def test_status_is_up_when_the_api_accepts_the_key(monkeypatch):
    body = _status(monkeypatch, None)
    assert body["state"] == "up" and body["detail"] == "The assistant is online."
    assert body["model"] == "claude-haiku-5-5" and body["configured"] is True


def test_status_says_when_no_key_is_configured(monkeypatch):
    body = _status(monkeypatch, None, key=None)
    assert body["state"] == "down" and body["configured"] is False
    assert "not configured" in body["detail"]


def test_status_is_down_without_leaking_the_key_or_the_cause(monkeypatch):
    body = _status(monkeypatch, LLMError("The model provider could not be reached."))
    assert body["state"] == "down" and body["configured"] is True
    assert "could not be reached" in body["detail"]
    assert "sk-test" not in json.dumps(body)


def test_status_is_public_and_probes_the_api_once_per_cache_window(monkeypatch):
    calls = []
    monkeypatch.setattr(route.ClaudeClient, "probe", lambda self: calls.append(1))
    monkeypatch.setenv("ANTHROPIC_API_KEY", "sk-test")
    monkeypatch.setattr(route, "_STATUS_CACHE", route.TTLCache(1, 15))
    anonymous = TestClient(app)
    for _ in range(5):
        assert anonymous.get("/api/assistant/status").json()["state"] == "up"
    assert len(calls) == 1


# ── The Claude client, against a fake SDK (no network) ───────────────────────


class _FakeStream:
    def __init__(self, chunks, stop_reason="end_turn", error=None):
        self.text_stream = iter(chunks)
        self._stop = stop_reason
        self._error = error

    def __enter__(self):
        if self._error is not None:
            raise self._error
        return self

    def __exit__(self, *exc):
        return False

    def get_final_message(self):
        from types import SimpleNamespace

        return SimpleNamespace(stop_reason=self._stop, stop_details=None)


def _claude(monkeypatch, stream: _FakeStream, key="sk-test"):
    """A ClaudeClient whose SDK client records the request and replays `stream`."""
    from types import SimpleNamespace

    from api.app.assistant import llm

    sent = {}

    def fake_stream(**kw):
        sent.update(kw)
        return stream

    fake = SimpleNamespace(messages=SimpleNamespace(stream=fake_stream))
    monkeypatch.setattr(llm.anthropic, "Anthropic", lambda **kw: fake)
    client = llm.ClaudeClient(
        model="claude-haiku-5-5",
        effort="medium",
        max_tokens=16000,
        timeout_s=10,
        api_key=key,
    )
    return client, sent


def test_claude_streams_text_with_the_system_prompt_apart(monkeypatch):
    client, sent = _claude(monkeypatch, _FakeStream(["Hel", "lo"]))
    history = [{"role": "user", "content": "a call"}]
    assert "".join(client.stream("SYSTEM", history)) == "Hello"
    assert sent["system"] == "SYSTEM" and sent["messages"] == history
    assert sent["model"] == "claude-haiku-5-5"
    # Rejected with a 400 on Haiku 5.5, and nothing about the user is sent.
    assert "temperature" not in sent and "metadata" not in sent
    assert sent["output_config"] == {"effort": "medium"}


@pytest.mark.parametrize(
    "stop, words", [("refusal", "declined"), ("max_tokens", "cut off")]
)
def test_claude_turns_a_refusal_or_a_truncation_into_an_error(monkeypatch, stop, words):
    client, _ = _claude(monkeypatch, _FakeStream(["partial"], stop_reason=stop))
    with pytest.raises(LLMError, match=words):
        list(client.stream("S", [{"role": "user", "content": "x"}]))


def test_claude_without_a_key_is_not_configured(monkeypatch):
    client, _ = _claude(monkeypatch, _FakeStream([]), key=None)
    with pytest.raises(LLMNotConfigured):
        list(client.stream("S", [{"role": "user", "content": "x"}]))


def test_claude_api_errors_become_sentences_for_the_user(monkeypatch):
    import anthropic
    import httpx2

    req = httpx2.Request("POST", "https://api.anthropic.com/v1/messages")
    cases = [
        (anthropic.APIConnectionError(request=req), "could not be reached"),
        (
            anthropic.RateLimitError(
                "slow down", response=httpx2.Response(429, request=req), body=None
            ),
            "limiting requests",
        ),
        (
            anthropic.AuthenticationError(
                "bad key sk-secret",
                response=httpx2.Response(401, request=req),
                body=None,
            ),
            "credentials were refused",
        ),
    ]
    for error, words in cases:
        client, _ = _claude(monkeypatch, _FakeStream([], error=error))
        with pytest.raises(LLMError, match=words) as exc:
            list(client.stream("S", [{"role": "user", "content": "x"}]))
        assert "sk-secret" not in str(exc.value)


# ── Prepaid credit used up: generic message, status down (issue #337) ────────


def _billing_errors():
    import anthropic
    import httpx2

    req = httpx2.Request("POST", "https://api.anthropic.com/v1/messages")

    def resp(code):
        return httpx2.Response(code, request=req)

    return [
        anthropic.BadRequestError(
            "Your credit balance is too low to access the Anthropic API.",
            response=resp(400),
            body=None,
        ),
        anthropic.PaymentRequiredError(
            "payment required", response=resp(402), body=None
        ),
        # A RateLimitError subclass: must not read as "retry in a minute".
        anthropic.EnforcedSpendLimitReachedError(
            "monthly limit", response=resp(429), body=None
        ),
    ]


@pytest.fixture
def no_billing_block():
    from api.app.assistant import llm

    llm.clear_billing_block()
    yield
    llm.clear_billing_block()


@pytest.mark.parametrize("error", _billing_errors(), ids=["credit", "402", "spend"])
def test_billing_error_says_unavailable_and_blocks_the_status(
    monkeypatch, no_billing_block, error
):
    from api.app.assistant import llm

    client, _ = _claude(monkeypatch, _FakeStream([], error=error))
    with pytest.raises(llm.LLMBillingBlocked, match="temporarily unavailable") as exc:
        list(client.stream("S", [{"role": "user", "content": "x"}]))
    assert "credit" not in str(exc.value) and "limit" not in str(exc.value)
    assert llm.billing_blocked()
    # The free models probe would still answer: the block is what says down.
    with pytest.raises(llm.LLMBillingBlocked):
        client.probe()


def test_status_is_down_while_billing_blocks_then_comes_back(
    monkeypatch, no_billing_block
):
    from api.app.assistant import llm

    monkeypatch.setenv("ANTHROPIC_API_KEY", "sk-test")
    fake_models = type("M", (), {"retrieve": lambda self, model: None})()
    fake = type(
        "C",
        (),
        {"models": fake_models, "with_options": lambda self, **kw: self},
    )()
    monkeypatch.setattr(llm.anthropic, "Anthropic", lambda **kw: fake)

    def status():
        monkeypatch.setattr(route, "_STATUS_CACHE", route.TTLCache(1, 15))
        return TestClient(app).get("/api/assistant/status").json()

    assert status()["state"] == "up"
    llm._block_for_billing()
    body = status()
    assert body["state"] == "down" and body["configured"] is True
    assert body["detail"] == "The assistant is temporarily unavailable. Retry later."
    llm.clear_billing_block()
    assert status()["state"] == "up"
