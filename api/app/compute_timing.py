"""Where a pricing's server time goes: reading market data, calibrating a
model to it, and the engine itself (the Monte-Carlo run, the tree, the
closed form).

`valuation.price` opens a collector around each pricing; the code that reads
the database, calibrates or calls the engine marks its span with `phase()`.
Phases are exclusive: a calibration run inside a market load counts as
calibration only, so the phases never add up to more than the wall time.
What no phase covers (request handling, script parsing, Python glue) is the
remainder the response reports as `other_ms`.

Outside a collector (a replay of greeks by bump, a test calling a pricer
directly) `phase()` costs one context-variable read and records nothing.
"""

from __future__ import annotations

import time
from contextlib import contextmanager
from contextvars import ContextVar
from dataclasses import dataclass, field
from typing import Iterator, Literal, Optional

Phase = Literal["market", "calibration", "engine"]


@dataclass
class _Collector:
    totals: dict[str, float] = field(
        default_factory=lambda: {"market": 0.0, "calibration": 0.0, "engine": 0.0}
    )
    #: Open phases, innermost last: [name, start, time spent in children].
    stack: list[list] = field(default_factory=list)


_current: ContextVar[Optional[_Collector]] = ContextVar(
    "compute_timing_collector", default=None
)


@contextmanager
def collect() -> Iterator[dict[str, float]]:
    """Collects the phases of what runs inside; the yielded dict holds their
    seconds once the block exits."""
    collector = _Collector()
    token = _current.set(collector)
    try:
        yield collector.totals
    finally:
        _current.reset(token)


@contextmanager
def phase(name: Phase) -> Iterator[None]:
    collector = _current.get()
    if collector is None:
        yield
        return
    frame = [name, time.perf_counter(), 0.0]
    collector.stack.append(frame)
    try:
        yield
    finally:
        collector.stack.pop()
        elapsed = time.perf_counter() - frame[1]
        collector.totals[name] += elapsed - frame[2]
        if collector.stack:
            collector.stack[-1][2] += elapsed
