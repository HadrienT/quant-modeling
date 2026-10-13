"""Router — the payoff-scripting assistant (chat with a local LLM).

POST /api/assistant/scripting/chat   (Server-Sent Events)
GET  /api/assistant/status           is the model server up, and with which model

The model is the llama-server already running on the host (AgenticEnv); this
route adds what a raw chat lacks: the DSL prompt, and a check of every script
the model writes against the real parser before the user sees it
(assistant/agent.py). One GPU serves everything, so concurrent chats are
capped and the surplus is told to retry rather than queued.

The production app is public through the tunnel and this route spends the
owner's GPU, so a visitor who is not signed in only gets a few questions a day
(assistant/quota.py, issue #175): past them the route answers 429 with the
code `anonymous_quota_exhausted`, which the page turns into a sign-in prompt.
Signed-in users are not counted, and one slot is always kept for them.
"""

from __future__ import annotations

import json
import threading
import time
from datetime import datetime, timezone
from pathlib import PurePosixPath
from typing import Callable, Iterator, Optional

from fastapi import APIRouter, Depends, HTTPException
from fastapi.responses import StreamingResponse
from starlette.background import BackgroundTask

from ..assistant.agent import run_assistant
from ..assistant import quota
from ..assistant.llm import LlamaServerClient, LLMError
from ..assistant.schemas import AssistantStatusResponse, ScriptingChatRequest
from ..audit import emit
from ..audit.payloads import AssistantChatPayload, AssistantOutcome
from ..auth import optional_user
from ..cache import TTLCache
from ..request_context import current_ip_hash

router = APIRouter(prefix="/api/assistant", tags=["assistant"])

_MAX_CONCURRENT_CHATS = 2
_slots = threading.BoundedSemaphore(_MAX_CONCURRENT_CHATS)
# Visitors who are not signed in never hold every slot: one stays free for
# signed-in users.
_anonymous_slots = threading.BoundedSemaphore(_MAX_CONCURRENT_CHATS - 1)
_anonymous_quota = quota.from_env()

ANONYMOUS_QUOTA_CODE = "anonymous_quota_exhausted"


class AnonymousQuotaExhausted(HTTPException):
    """A 429 the page can tell apart from "both slots busy": `code` replaces
    the generic `rate_limited` in the error body (main.py)."""

    code = ANONYMOUS_QUOTA_CODE

    def __init__(self, scope: str):
        super().__init__(
            status_code=429,
            detail=(
                "You have used today's free questions. Sign in to continue."
                if scope == "visitor"
                else "Today's free questions for visitors are used up. "
                "Sign in to continue."
            ),
        )


# The status is public (the page shows it before anyone signs in) and each
# probe is a request to the model server: answered from a short cache, so the
# route cannot be used to hammer it.
_STATUS_CACHE = TTLCache[str, AssistantStatusResponse](max_size=1, ttl_seconds=15)


def _model_key(name: str) -> str:
    """A model's name whatever form the server lists it in: a bare id, a file
    name or a path to a .gguf."""
    return PurePosixPath(name).name.lower().removesuffix(".gguf")


def _probe() -> AssistantStatusResponse:
    client = LlamaServerClient.from_env()
    now = datetime.now(timezone.utc)
    try:
        serving = client.served_models()
    except LLMError:
        # The reason (an internal address, a socket error) stays in the logs.
        return AssistantStatusResponse(
            state="down",
            model=client.model,
            detail="The model server does not answer: the assistant is offline.",
            checked_at=now,
        )
    wanted = _model_key(client.model)
    # A server that lists nothing cannot be told apart from a right one.
    if not serving or any(_model_key(m) == wanted for m in serving):
        return AssistantStatusResponse(
            state="up",
            model=client.model,
            serving=serving,
            detail="The assistant is online.",
            checked_at=now,
        )
    return AssistantStatusResponse(
        state="wrong_model",
        model=client.model,
        serving=serving,
        detail=(
            f"The model server is running {', '.join(serving)}, not the "
            f"assistant's model ({client.model}): answers would come from that "
            "model."
        ),
        checked_at=now,
    )


@router.get("/status", response_model=AssistantStatusResponse)
def assistant_status_endpoint() -> AssistantStatusResponse:
    cached = _STATUS_CACHE.get("status")
    if cached is not None:
        return cached
    status = _probe()
    _STATUS_CACHE.set("status", status)
    return status


def _sse(event: dict) -> str:
    return f"data: {json.dumps(event, ensure_ascii=False)}\n\n"


def _once(release: Callable[[], None]) -> Callable[[], None]:
    """A slot must be freed exactly once, whichever of the stream's `finally`
    or the response's background task gets there first (a generator that was
    never started never runs its `finally`)."""
    lock = threading.Lock()
    done = False

    def run() -> None:
        nonlocal done
        with lock:
            if done:
                return
            done = True
        release()

    return run


def _stream(
    req: ScriptingChatRequest,
    release: Callable[[], None],
    username: Optional[str],
    remaining: Optional[int] = None,
) -> Iterator[str]:
    start = time.perf_counter()
    proposed = valid = 0
    failed = False
    try:
        if remaining is not None:
            yield _sse({"type": "quota", "remaining": remaining})
        events = run_assistant(
            model=LlamaServerClient.from_env(),
            history=[m.model_dump() for m in req.messages],
            valuation_date=req.valuation_date.isoformat(),
            day_count=req.day_count,
            current_script=req.current_script,
            last_error=req.last_error,
        )
        for event in events:
            # A `retry` is a draft the parser rejected; a `script` is the one
            # shown to the user, valid or not.
            if event["type"] in ("script", "retry"):
                proposed += 1
                valid += event["type"] == "script" and bool(event.get("valid"))
            failed = failed or event["type"] == "error"
            yield _sse(event)
    finally:
        release()
        # Metadata only, never the conversation (blueprint WP 18 §7).
        emit(
            "assistant.chat",
            AssistantChatPayload(
                outcome=(
                    AssistantOutcome.FAILED if failed else AssistantOutcome.COMPLETED
                ),
                turns=len(req.messages),
                has_script=bool(req.current_script),
                has_error=bool(req.last_error),
                duration_ms=(time.perf_counter() - start) * 1000,
                scripts_proposed=proposed,
                scripts_valid=valid,
            ),
            username=username,
        )


@router.post(
    "/scripting/chat",
    response_class=StreamingResponse,
    responses={
        200: {
            "description": "text/event-stream of ScriptingChatEvent payloads.",
            "content": {"text/event-stream": {"schema": {"type": "string"}}},
        },
        429: {
            "description": (
                "`rate_limited`: the assistant slots are busy, retry. "
                "`anonymous_quota_exhausted`: a visitor who is not signed in "
                "has used the day's free questions, sign in."
            )
        },
    },
)
def scripting_chat_endpoint(
    req: ScriptingChatRequest, user: Optional[str] = Depends(optional_user)
) -> StreamingResponse:
    held = [_slots] if user else [_anonymous_slots, _slots]
    acquired: list[threading.BoundedSemaphore] = []

    def free() -> None:
        for slot in acquired:
            slot.release()

    for slot in held:
        if not slot.acquire(blocking=False):
            free()
            raise HTTPException(
                status_code=429,
                detail="The assistant is busy with other requests. Retry in a moment.",
            )
        acquired.append(slot)
    remaining: Optional[int] = None
    if user is None:
        # Counted once a slot is held: a "busy" answer costs the visitor nothing.
        try:
            remaining = _anonymous_quota.consume(current_ip_hash())
        except quota.QuotaExhausted as exc:
            free()
            raise AnonymousQuotaExhausted(exc.scope) from None
    release = _once(free)
    return StreamingResponse(
        _stream(req, release, user, remaining),
        media_type="text/event-stream",
        headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"},
        background=BackgroundTask(release),
    )
