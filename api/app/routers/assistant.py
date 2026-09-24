"""Router — the payoff-scripting assistant (chat with a local LLM).

POST /api/assistant/scripting/chat   (Server-Sent Events)

The model is the llama-server already running on the host (AgenticEnv); this
route adds what a raw chat lacks: the DSL prompt, and a check of every script
the model writes against the real parser before the user sees it
(assistant/agent.py). One GPU serves everything, so concurrent chats are
capped and the surplus is told to retry rather than queued. Login is required
(JWT, like the portfolio routes): the production app is public through the
tunnel, and this route spends the owner's GPU.
"""

from __future__ import annotations

import json
import threading
import time
from typing import Callable, Iterator

from fastapi import APIRouter, Depends, HTTPException
from fastapi.responses import StreamingResponse
from starlette.background import BackgroundTask

from ..assistant.agent import run_assistant
from ..assistant.llm import LlamaServerClient
from ..assistant.schemas import ScriptingChatRequest
from ..audit import emit
from ..audit.payloads import AssistantChatPayload, AssistantOutcome
from ..auth import require_user

router = APIRouter(prefix="/api/assistant", tags=["assistant"])

_MAX_CONCURRENT_CHATS = 2
_slots = threading.BoundedSemaphore(_MAX_CONCURRENT_CHATS)


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
    req: ScriptingChatRequest, release: Callable[[], None], username: str
) -> Iterator[str]:
    start = time.perf_counter()
    proposed = valid = 0
    failed = False
    try:
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
        401: {"description": "Not signed in."},
        429: {"description": "Both assistant slots are busy."},
    },
)
def scripting_chat_endpoint(
    req: ScriptingChatRequest, user: str = Depends(require_user)
) -> StreamingResponse:
    if not _slots.acquire(blocking=False):
        raise HTTPException(
            status_code=429,
            detail="The assistant is busy with other requests. Retry in a moment.",
        )
    release = _once(_slots.release)
    return StreamingResponse(
        _stream(req, release, user),
        media_type="text/event-stream",
        headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"},
        background=BackgroundTask(release),
    )
