"""`emit()` — the one entry point application code calls to record an audit
event (WP §5.1). Contract: it never raises and never blocks the request; a
transport failure counts against `qm_audit_dropped_total` instead of surfacing
to the caller.
"""

from __future__ import annotations

import sys

from pydantic import BaseModel

from ..request_context import current_request_id, current_trace_id, current_username
from .envelope import Event
from .metrics import audit_dropped_total
from .sinks import get_sink
from .. import telemetry

__all__ = ["emit"]


def emit(
    event_type: str,
    payload: BaseModel,
    *,
    version: int = 1,
    username: str | None = None,
) -> None:
    """`payload` is always a typed model (never a free dict) — see payloads.py."""
    try:
        event = Event.create(
            event_type,
            payload,
            version=version,
            request_id=current_request_id(),
            trace_id=current_trace_id(),
            username=username if username is not None else current_username(),
        )
        get_sink().publish(event)
        _count(event)
    except Exception as exc:  # noqa: BLE001 — emit() must never break the caller
        audit_dropped_total.inc()
        print(
            f"qm_audit_dropped_total: emit({event_type}) failed: {exc}", file=sys.stderr
        )


def _count(event: Event) -> None:
    """The metric side of an audit event (WP §5.4): the two paths stay
    parallel — the event goes to Kafka, the count to the OTel Collector."""
    if event.type == "data.fallback":
        telemetry.data_fallback.add(1, {"kind": str(event.payload.get("kind"))})
    elif event.type.startswith("auth."):
        telemetry.auth_events.add(1, {"outcome": str(event.payload.get("outcome"))})
