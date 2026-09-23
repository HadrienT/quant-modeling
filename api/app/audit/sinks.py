"""Event transports (WP §5.1) — same shape as `storage.py`: a `Protocol`,
pluggable implementations, one chosen by `QM_AUDIT_SINK`.
"""

from __future__ import annotations

import os
import sys
import threading
from datetime import datetime, timezone
from pathlib import Path
from typing import List, Protocol

from .envelope import Event
from .metrics import audit_dropped_total


class EventSink(Protocol):
    def publish(self, event: Event) -> None:
        """Must never raise and must not block the caller."""
        ...

    def close(self) -> None: ...


class SpoolSink:
    """Appends one JSON line per event to a daily file under `QM_AUDIT_SPOOL_DIR`
    (`spool/*.jsonl`). The sole transport of lot 18a; from lot 18b on it is
    `KafkaSink`'s safety net for a broker that is down or a delivery that
    failed."""

    def __init__(self, spool_dir: str | os.PathLike) -> None:
        self._dir = Path(spool_dir)
        self._dir.mkdir(parents=True, exist_ok=True)
        self._lock = threading.Lock()

    def _path(self) -> Path:
        day = datetime.now(timezone.utc).strftime("%Y-%m-%d")
        return self._dir / f"{day}.jsonl"

    def publish(self, event: Event) -> None:
        try:
            line = event.model_dump_json() + "\n"
        except Exception as exc:  # noqa: BLE001 — emit() must never break the caller
            audit_dropped_total.inc()
            print(
                f"qm_audit_dropped_total: could not serialize {event.type}: {exc}",
                file=sys.stderr,
            )
            return
        try:
            with self._lock, self._path().open("a", encoding="utf-8") as fh:
                fh.write(line)
        except OSError as exc:
            audit_dropped_total.inc()
            print(f"qm_audit_dropped_total: spool write failed: {exc}", file=sys.stderr)

    def close(self) -> None:
        pass


class InMemorySink:
    """For tests: collects every event published so far."""

    def __init__(self) -> None:
        self.events: List[Event] = []

    def publish(self, event: Event) -> None:
        self.events.append(event)

    def close(self) -> None:
        pass


class KafkaSink(SpoolSink):
    """A façade, not yet wired (WP §5.1): the real `confluent-kafka` producer
    — `acks=all`, idempotence, delivery callbacks, republishing the spool —
    arrives in lot 18b. Until then `KafkaSink` behaves exactly like
    `SpoolSink`, so selecting it today (`QM_AUDIT_SINK=kafka`) is
    forward-compatible rather than a silent no-op, and lot 18b only has to
    override `publish`/`close`, not touch any caller."""


_INSTANCE: EventSink | None = None


def get_sink() -> EventSink:
    global _INSTANCE
    if _INSTANCE is not None:
        return _INSTANCE
    backend = os.getenv("QM_AUDIT_SINK", "spool").lower()
    spool_dir = os.getenv(
        "QM_AUDIT_SPOOL_DIR",
        str(Path(os.getenv("LOG_DIR", "/tmp/quantmodeling")) / "spool"),
    )
    _INSTANCE = KafkaSink(spool_dir) if backend == "kafka" else SpoolSink(spool_dir)
    return _INSTANCE


def set_sink_for_testing(sink: EventSink) -> None:
    global _INSTANCE
    _INSTANCE = sink


def reset_sink_for_testing() -> None:
    global _INSTANCE
    _INSTANCE = None
