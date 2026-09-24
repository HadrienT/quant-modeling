"""In-process state of the audit trail itself (WP §5.1: `qm_audit_dropped_total`,
`qm_audit_spool_depth`).

These are plain values the transports update; `telemetry.py` exports them over
OTLP as observable instruments (lot 18d), so they exist — at 0 — from the
moment the API starts, which is what lets an `increase()` alert fire on the
very first drop (quant-platform ADR-013 §2). Every drop is also printed to
stderr — "an event dropped must be visible" (WP §5.1), even without a
dashboard.
"""

from __future__ import annotations

import threading


class Counter:
    def __init__(self) -> None:
        self._value = 0
        self._lock = threading.Lock()

    def inc(self, n: int = 1) -> None:
        with self._lock:
            self._value += n

    @property
    def value(self) -> int:
        return self._value

    def reset_for_testing(self) -> None:
        with self._lock:
            self._value = 0


class Gauge:
    def __init__(self) -> None:
        self._value = 0

    def set(self, value: int) -> None:
        self._value = value

    @property
    def value(self) -> int:
        return self._value


audit_dropped_total = Counter()
audit_spool_depth = Gauge()
