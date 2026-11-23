"""Stop a pricing nobody waits for any more (issue #347).

A pricing route answers 408 after its timeout, but Python cannot stop the
worker thread from outside, and the C++ engines do not poll for a stop: the
computation used to run to its end, holding its memory and its CPU or GPU,
while the user retried beside it -- what OOM-killed production on
2026-11-20. So the route gives the thread a flag, sets it when it answers
408, and the pricing checks it at the points where it can stop cleanly:
between the runs a pricing chains (a precision target's pilot, cost and
final runs; a price run and a risks run; a convergence ladder; the bumps of
the greeks and of a risk profile). A run already inside the C++ finishes;
no further one starts. So a pricing that plans its own runs also learns when
the route gives up (time_left), and sizes them to end before it.
"""

from __future__ import annotations

import contextvars
import threading
import time
from typing import Callable, Optional, TypeVar

_T = TypeVar("_T")

#: The flag of the pricing running in this thread (None outside a route).
_CURRENT: contextvars.ContextVar[Optional[threading.Event]] = contextvars.ContextVar(
    "qm_pricing_cancel", default=None
)


#: When the route answers 408 (time.perf_counter()), None outside a route.
_DEADLINE: contextvars.ContextVar[Optional[float]] = contextvars.ContextVar(
    "qm_pricing_deadline", default=None
)


class Cancelled(Exception):
    """The route already answered (408): the pricing stops at this point."""


def check() -> None:
    """Raise Cancelled if the pricing in progress was abandoned."""
    flag = _CURRENT.get()
    if flag is not None and flag.is_set():
        raise Cancelled("the pricing was abandoned after its timeout")


def time_left() -> Optional[float]:
    """Seconds before the route answers 408, None outside a route. A pricing
    that plans its own runs (a precision target) plans within it: a run
    sized past it would outlive the answer, and no check can stop a run
    already in the C++."""
    deadline = _DEADLINE.get()
    return None if deadline is None else deadline - time.perf_counter()


def run_with(
    flag: threading.Event,
    fn: Callable[..., _T],
    *args,
    deadline: Optional[float] = None,
) -> _T:
    """`fn(*args)` with `flag` as the pricing's cancellation flag and
    `deadline` (time.perf_counter()) as when its route gives up."""
    token = _CURRENT.set(flag)
    token_d = _DEADLINE.set(deadline) if deadline is not None else None
    try:
        return fn(*args)
    finally:
        if token_d is not None:
            _DEADLINE.reset(token_d)
        _CURRENT.reset(token)


def bind(fn: Callable[..., _T]) -> Callable[..., _T]:
    """`fn` for another thread (a ThreadPoolExecutor's, which does not carry
    the caller's context): it checks the caller's flag before it runs."""
    flag, deadline = _CURRENT.get(), _DEADLINE.get()
    if flag is None:
        return fn

    def bound(*args, **kwargs):
        if flag.is_set():
            raise Cancelled("the pricing was abandoned after its timeout")
        return run_with(flag, lambda: fn(*args, **kwargs), deadline=deadline)

    return bound
