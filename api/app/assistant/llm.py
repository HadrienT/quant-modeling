"""Client for the scripting assistant's model: Claude, through the Anthropic API.

Only what the assistant needs: a streamed answer yielding text deltas, and a
cheap probe for the status route. No tool calling: the agent loop checks the
model's script with the real parser instead (agent.py).

Configuration (issue #337):
- ANTHROPIC_API_KEY   the key; never committed (production secret in .env).
- QM_LLM_MODEL        default claude-haiku-5-5.
- QM_LLM_EFFORT       low | medium | high | xhigh | max; default medium.
- QM_LLM_MAX_TOKENS   output cap per call, thinking included; default 16000.
- QM_LLM_TIMEOUT_S    per-request timeout of the SDK; default 120.

Privacy: a request carries the system prompt, the conversation the user typed,
the script in their editor and its last error. Nothing else: no user name,
email or IP (no `metadata` is sent).
"""

from __future__ import annotations

import logging
import os
import threading
import time
from dataclasses import dataclass
from typing import Iterator, Protocol, Sequence

import anthropic

Message = dict[str, str]

log = logging.getLogger(__name__)

DEFAULT_MODEL = "claude-haiku-5-5"


class LLMError(RuntimeError):
    """The model could not answer. The message is shown to the user as is, so
    it never carries an internal detail (the reason goes to the logs)."""


class LLMNotConfigured(LLMError):
    """No API key: the assistant is switched off on this server."""


class LLMBillingBlocked(LLMError):
    """The API account cannot pay for requests: prepaid credit used up, a
    spend limit reached, or the organization on hold. Visitors are only told
    the assistant is unavailable; the cause is logged as an error."""


# The API account is prepaid: once the credit is gone every chat fails, while
# the free GET /v1/models probe keeps answering. A billing error therefore
# marks the assistant down for this long, so the status route (and the page)
# say so instead of letting visitors type into a chat that cannot answer.
BILLING_BLOCK_S = 600.0
_billing_lock = threading.Lock()
_billing_blocked_until = 0.0


def _block_for_billing() -> None:
    global _billing_blocked_until
    with _billing_lock:
        _billing_blocked_until = time.monotonic() + BILLING_BLOCK_S


def billing_blocked() -> bool:
    with _billing_lock:
        return time.monotonic() < _billing_blocked_until


def clear_billing_block() -> None:
    global _billing_blocked_until
    with _billing_lock:
        _billing_blocked_until = 0.0


_UNAVAILABLE = "The assistant is temporarily unavailable."


class ChatModel(Protocol):
    model: str

    def stream(self, system: str, messages: Sequence[Message]) -> Iterator[str]:
        """Text deltas of one answer. `messages` alternate user / assistant
        and end with a user message; `system` is the system prompt."""
        ...


def _api_key() -> str | None:
    return os.getenv("ANTHROPIC_API_KEY") or None


def _is_billing(exc: anthropic.APIError) -> bool:
    """Whether the account cannot pay. Typed classes first; an exhausted
    prepaid balance has no class of its own (a 400 invalid_request_error whose
    message names the credit balance), hence the one text match."""
    if isinstance(
        exc,
        (
            anthropic.PaymentRequiredError,  # 402, linked-org billing included
            anthropic.EnforcedSpendLimitReachedError,  # 429, before RateLimitError
            anthropic.ContractSpendLimitReachedError,
            anthropic.OrganizationOnHoldError,
        ),
    ):
        return True
    return (
        isinstance(exc, anthropic.BadRequestError)
        and "credit balance" in str(exc).lower()
    )


def _translate(exc: anthropic.APIError) -> LLMError:
    """An SDK error as a sentence for the user, the detail kept in the logs.
    Most specific first: the SDK has already retried what can be retried."""
    if _is_billing(exc):
        _block_for_billing()
        log.error(
            "assistant: Anthropic API billing error (credit used up, spend "
            "limit or account on hold); assistant marked down for %.0f s: %r",
            BILLING_BLOCK_S,
            exc,
        )
        return LLMBillingBlocked(_UNAVAILABLE)
    log.warning("assistant: Anthropic API error: %r", exc)
    if isinstance(
        exc, (anthropic.AuthenticationError, anthropic.PermissionDeniedError)
    ):
        return LLMError("The assistant's API credentials were refused.")
    if isinstance(exc, anthropic.RateLimitError):
        return LLMError("The model provider is limiting requests. Retry in a minute.")
    if isinstance(exc, anthropic.APIStatusError) and exc.status_code < 500:
        return LLMError("The model provider rejected the request.")
    if isinstance(exc, anthropic.APIStatusError):
        return LLMError("The model provider is unavailable. Retry in a moment.")
    if isinstance(exc, anthropic.APIConnectionError):
        return LLMError("The model provider could not be reached.")
    return LLMError("The model provider failed to answer.")


@dataclass(frozen=True)
class ClaudeClient:
    model: str
    effort: str
    max_tokens: int
    timeout_s: float
    api_key: str | None

    @classmethod
    def from_env(cls) -> "ClaudeClient":
        return cls(
            model=os.getenv("QM_LLM_MODEL", DEFAULT_MODEL),
            effort=os.getenv("QM_LLM_EFFORT", "medium"),
            # Adaptive thinking is on by default on Haiku 5.5 and its tokens
            # count against this cap: 2 048 (the llama-server value) would cut
            # answers off. At $0.50 per million output tokens, 16 000 is
            # under a cent per call.
            max_tokens=int(os.getenv("QM_LLM_MAX_TOKENS", "16000")),
            timeout_s=float(os.getenv("QM_LLM_TIMEOUT_S", "120")),
            api_key=_api_key(),
        )

    @property
    def configured(self) -> bool:
        return self.api_key is not None

    def _client(self, **options) -> anthropic.Anthropic:
        if self.api_key is None:
            raise LLMNotConfigured(
                "The assistant is not configured on this server (no API key)."
            )
        return anthropic.Anthropic(
            api_key=self.api_key, timeout=self.timeout_s, **options
        )

    def probe(self) -> None:
        """Raise LLMError unless the key is accepted and the model exists.
        GET /v1/models/{id}: free, no tokens spent. Short timeout, no retry:
        this is asked for while a page loads."""
        client = self._client(max_retries=0)
        if billing_blocked():
            raise LLMBillingBlocked(_UNAVAILABLE)
        try:
            client.with_options(timeout=5.0).models.retrieve(self.model)
        except anthropic.NotFoundError as exc:
            log.warning("assistant: model %s unknown to the API: %r", self.model, exc)
            raise LLMError(f"The model {self.model} is not available.") from exc
        except anthropic.APIError as exc:
            raise _translate(exc) from exc

    def stream(self, system: str, messages: Sequence[Message]) -> Iterator[str]:
        client = self._client()
        try:
            with client.messages.stream(
                model=self.model,
                max_tokens=self.max_tokens,
                system=system,
                messages=list(messages),
                # Thinking stays adaptive (the default); its blocks are not
                # part of text_stream, so the chat only shows the answer.
                output_config={"effort": self.effort},
            ) as stream:
                yield from stream.text_stream
                final = stream.get_final_message()
        except anthropic.APIError as exc:
            raise _translate(exc) from exc
        if final.stop_reason == "refusal":
            # Haiku 5.5 has no server-side fallback model.
            log.info(
                "assistant: refusal (%s)",
                final.stop_details.category if final.stop_details else None,
            )
            raise LLMError("The model declined to answer this request.")
        if final.stop_reason == "max_tokens":
            raise LLMError(
                "The answer was cut off at its length limit. Ask for a smaller "
                "change or split the question."
            )
