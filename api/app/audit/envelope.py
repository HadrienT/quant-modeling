"""The event envelope — common to every event (contract §1, WP §4.1).

The envelope is a plain typed model; the payload is not — it is owned by a
per-event-type model in `payloads.py`. That split is what makes "never a
password, token, or Authorization header" a property of construction rather
than a filter applied at emission: a field that was never declared on a
payload model can never reach the wire.
"""

from __future__ import annotations

import os
import time
import uuid
from datetime import datetime, timezone
from typing import Any

from pydantic import BaseModel, Field


def uuid7() -> str:
    """RFC 9562 UUID version 7: time-ordered.

    Used as `event_id`, the sink's idempotency key (contract §4: at-least-once
    delivery + `ON CONFLICT DO NOTHING` on the primary key = exactly-once in
    effect). The standard library gains `uuid.uuid7()` only in Python 3.14; the
    API runs on 3.12/3.13, hence this small implementation.
    """
    unix_ts_ms = int(time.time() * 1000)
    rand = os.urandom(10)
    raw = bytearray(unix_ts_ms.to_bytes(6, "big") + rand)
    raw[6] = (raw[6] & 0x0F) | 0x70  # version 7 in the top nibble of byte 6
    raw[8] = (raw[8] & 0x3F) | 0x80  # variant 0b10 in the top 2 bits of byte 8
    return str(uuid.UUID(bytes=bytes(raw)))


def _occurred_at_now() -> str:
    now = datetime.now(timezone.utc)
    return now.strftime("%Y-%m-%dT%H:%M:%S.") + f"{now.microsecond // 1000:03d}Z"


def _lib_build_sha() -> str:
    """The native wheel's build stamp (WP 18a task 6), or "unknown" before it
    is wired / when the module stub stands in for tests and CI."""
    try:
        import quantmodeling as qm

        return str(qm.build_sha())
    except Exception:  # noqa: BLE001 — a missing/stubbed module must not break emit()
        return "unknown"


class Producer(BaseModel):
    service: str = "quant-modeling-api"
    git_sha: str
    lib_build: str


class Event(BaseModel):
    event_id: str = Field(default_factory=uuid7)
    type: str
    version: int = 1
    occurred_at: str = Field(default_factory=_occurred_at_now)
    request_id: str | None = None
    trace_id: str | None = None
    username: str | None = None
    producer: Producer
    payload: dict[str, Any]

    @classmethod
    def create(
        cls,
        event_type: str,
        payload: BaseModel,
        *,
        version: int = 1,
        request_id: str | None = None,
        trace_id: str | None = None,
        username: str | None = None,
    ) -> "Event":
        return cls(
            type=event_type,
            version=version,
            request_id=request_id,
            trace_id=trace_id,
            username=username,
            producer=Producer(
                git_sha=os.getenv("COMMIT_SHA", "dev"),
                lib_build=_lib_build_sha(),
            ),
            payload=payload.model_dump(mode="json"),
        )
