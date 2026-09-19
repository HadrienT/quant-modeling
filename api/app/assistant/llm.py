"""Minimal client for an OpenAI-compatible chat endpoint (llama-server).

Only what the assistant needs: a streamed chat completion yielding text
deltas. No tool calling — the agent loop checks the model's script with the
real parser instead (agent.py), which does not depend on how well a local
model follows a tool-call protocol.
"""

from __future__ import annotations

import json
import os
from dataclasses import dataclass
from typing import Iterator, Protocol, Sequence

import requests

Message = dict[str, str]


class LLMError(RuntimeError):
    """The model server is unreachable, slow, or answered nonsense."""


class ChatModel(Protocol):
    def stream(self, messages: Sequence[Message]) -> Iterator[str]: ...


@dataclass(frozen=True)
class LlamaServerClient:
    base_url: str
    model: str
    timeout_s: float
    max_tokens: int
    temperature: float

    @classmethod
    def from_env(cls) -> "LlamaServerClient":
        # QM_LLM_BASE_URL: from the host, llama-server itself
        # (http://127.0.0.1:8000/v1); from a container, the llama-bridge
        # socket on the Docker gateway (http://172.17.0.1:8001/v1).
        return cls(
            base_url=os.getenv("QM_LLM_BASE_URL", "http://127.0.0.1:8000/v1").rstrip(
                "/"
            ),
            model=os.getenv("QM_LLM_MODEL", "Qwen3-Coder-30B-A3B-Instruct"),
            timeout_s=float(os.getenv("QM_LLM_TIMEOUT_S", "120")),
            max_tokens=int(os.getenv("QM_LLM_MAX_TOKENS", "2048")),
            # Low but not zero: a script is code, but "ask a clarifying
            # question" and "explain" replies should not be robotic repeats.
            temperature=float(os.getenv("QM_LLM_TEMPERATURE", "0.2")),
        )

    def stream(self, messages: Sequence[Message]) -> Iterator[str]:
        body = {
            "model": self.model,
            "messages": list(messages),
            "stream": True,
            "max_tokens": self.max_tokens,
            "temperature": self.temperature,
        }
        try:
            resp = requests.post(
                f"{self.base_url}/chat/completions",
                json=body,
                stream=True,
                # (connect, read): the read timeout applies between chunks, so
                # a long answer is fine as long as tokens keep flowing.
                timeout=(5, self.timeout_s),
            )
        except requests.RequestException as exc:
            raise LLMError(f"model server unreachable: {exc}") from exc
        try:
            if resp.status_code != 200:
                raise LLMError(
                    f"model server answered HTTP {resp.status_code}: "
                    f"{resp.text[:200]}"
                )
            # Without this, requests decodes an SSE body (no charset in its
            # Content-Type) as latin-1 and every accent or dash is mangled.
            resp.encoding = "utf-8"
            for raw in resp.iter_lines(decode_unicode=True):
                if not raw or not raw.startswith("data:"):
                    continue
                payload = raw[len("data:") :].strip()
                if payload == "[DONE]":
                    return
                try:
                    choice = json.loads(payload)["choices"][0]
                except (ValueError, KeyError, IndexError) as exc:
                    raise LLMError(f"malformed stream chunk: {payload[:200]}") from exc
                delta = choice.get("delta", {}).get("content")
                if delta:
                    yield delta
        except requests.RequestException as exc:
            raise LLMError(f"model stream interrupted: {exc}") from exc
        finally:
            resp.close()
