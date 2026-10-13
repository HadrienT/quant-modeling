"""Free allowance of the scripting assistant for visitors who are not signed in
(issue #175).

A visitor without an account may ask a few questions a day, so that the
assistant can be tried without registering; past that, the route asks them to
sign in. Two counters, both reset at midnight UTC:

- one per visitor, keyed by the keyed hash of their address (the same hash the
  audit trail carries: the address itself is never held);
- one for all anonymous visitors together, so that many addresses cannot
  monopolise the GPU.

The counters live in this process: a restart hands everyone a fresh allowance,
which is acceptable for a courtesy quota and saves a table.
"""

from __future__ import annotations

import os
import threading
from datetime import date, datetime, timezone
from typing import Callable

# A request whose address could not be hashed (no client, no secret) shares
# one bucket rather than escaping the quota.
_UNKNOWN_VISITOR = "unknown"


class QuotaExhausted(Exception):
    """`scope` says which allowance ran out: the visitor's own or everyone's."""

    def __init__(self, scope: str):
        super().__init__(scope)
        self.scope = scope


def _today_utc() -> date:
    return datetime.now(timezone.utc).date()


class AnonymousQuota:
    def __init__(
        self,
        per_visitor: int,
        total: int,
        today: Callable[[], date] = _today_utc,
    ):
        self.per_visitor = per_visitor
        self.total = total
        self._today = today
        self._lock = threading.Lock()
        self._day = today()
        self._used: dict[str, int] = {}
        self._used_total = 0

    def consume(self, visitor: str | None) -> int:
        """Count one question for this visitor and return how many they have
        left today. Raises QuotaExhausted, counting nothing, when there is
        none left."""
        key = visitor or _UNKNOWN_VISITOR
        with self._lock:
            day = self._today()
            if day != self._day:
                self._day = day
                self._used.clear()
                self._used_total = 0
            used = self._used.get(key, 0)
            if used >= self.per_visitor:
                raise QuotaExhausted("visitor")
            if self._used_total >= self.total:
                raise QuotaExhausted("global")
            self._used[key] = used + 1
            self._used_total += 1
            return self.per_visitor - used - 1

    def reset(self) -> None:
        with self._lock:
            self._used.clear()
            self._used_total = 0


def from_env() -> AnonymousQuota:
    return AnonymousQuota(
        per_visitor=int(os.getenv("QM_ASSISTANT_ANON_DAILY", "10")),
        total=int(os.getenv("QM_ASSISTANT_ANON_DAILY_TOTAL", "300")),
    )
