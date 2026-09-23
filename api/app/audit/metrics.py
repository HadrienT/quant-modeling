"""In-process counters for the audit trail itself (WP §5.1: `qm_audit_dropped_total`).

A real Prometheus export is WP 18d (OTel Collector); until then this gives
`emit()` something to increment when a transport fails, and tests something to
assert on. Every increment is also printed to stderr — "an event dropped must
be visible" (WP §5.1), even without a dashboard.
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


audit_dropped_total = Counter()
