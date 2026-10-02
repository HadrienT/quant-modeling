from __future__ import annotations

from datetime import date, datetime, timezone
from typing import Literal

from pydantic import BaseModel, Field, field_validator


def _today_utc() -> date:
    return datetime.now(timezone.utc).date()


class ChatMessage(BaseModel):
    role: Literal["user", "assistant"]
    content: str = Field(..., min_length=1, max_length=8_000)


class ScriptingChatRequest(BaseModel):
    """One turn of the scripting assistant. The conversation lives in the
    browser: the client sends the whole history each time, plus what the editor
    currently holds, so the assistant reasons about the user's actual script."""

    messages: list[ChatMessage] = Field(..., min_length=1, max_length=30)
    current_script: str | None = Field(None, max_length=20_000)
    last_error: str | None = Field(
        None,
        max_length=4_000,
        description="The parser/pricing error currently shown next to the editor.",
    )
    valuation_date: date = Field(default_factory=_today_utc)
    day_count: Literal["ACT/365F", "ACT/360", "30/360", "ACT/ACT"] = "ACT/365F"

    @field_validator("messages")
    @classmethod
    def _ends_with_user(cls, v: list[ChatMessage]) -> list[ChatMessage]:
        if v[-1].role != "user":
            raise ValueError("the last message must come from the user")
        return v


class ScriptingChatEvent(BaseModel):
    """Documentation of the SSE payloads (`data: <json>` lines) — the route
    streams these; FastAPI cannot type a stream, so this model is what a
    client should read.

    - delta:  `text` — a chunk of the assistant's reply
    - retry:  the draft was rejected by the parser (`error`); a new draft
              follows, the client should discard the text streamed so far
    - script: the script found in the reply, with `valid` telling whether it
              passed the parser (`events` / `variables` when valid)
    - error:  the model server failed (`message`)
    - done:   end of stream
    """

    type: Literal["delta", "retry", "script", "error", "done"]
    text: str | None = None
    attempt: int | None = None
    script: str | None = None
    valid: bool | None = None
    error: str | None = None
    message: str | None = None
    events: list[dict] | None = None
    variables: list[str] | None = None


class AssistantStatusResponse(BaseModel):
    """Whether the assistant can answer right now.

    `up`: the model server answers and serves the assistant's model.
    `wrong_model`: it answers, but another model is loaded — a chat would be
    answered by that one. `down`: it does not answer."""

    state: Literal["up", "wrong_model", "down"]
    model: str = Field(..., description="The model the assistant is set up for.")
    serving: list[str] = Field(
        default_factory=list, description="What the model server reports."
    )
    detail: str = Field(..., description="One sentence for the page.")
    checked_at: datetime
