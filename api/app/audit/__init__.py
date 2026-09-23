"""Audit trail — blueprint WP 18a (`blueprint/wp/18-observability.md`).

Same idiom as `storage.py`: a protocol (`EventSink`), pluggable implementations
selected by an environment variable, and one call application code uses without
knowing the transport (`emit`, `record_fallback`). This package is the entire
producer side of the contract with the `quant-platform` repo
(`~/quant-platform/docs/contract.md`); it works with zero infrastructure
(`SpoolSink` is the only transport this lot needs) and swaps to Kafka in WP 18b
without any caller changing.
"""

from .emit import emit
from .envelope import Event, Producer, uuid7
from .fallback import record_fallback
from .payloads import (
    AuthEventPayload,
    AuthOutcome,
    FallbackKind,
    FallbackPayload,
    HttpAccessPayload,
)
from .sinks import EventSink, InMemorySink, KafkaSink, SpoolSink, get_sink

__all__ = [
    "AuthEventPayload",
    "AuthOutcome",
    "Event",
    "EventSink",
    "FallbackKind",
    "FallbackPayload",
    "HttpAccessPayload",
    "InMemorySink",
    "KafkaSink",
    "Producer",
    "SpoolSink",
    "emit",
    "get_sink",
    "record_fallback",
    "uuid7",
]
