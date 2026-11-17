"""Long backtest runs as tasks, with progress and cancellation
(blueprint/wp/24-backtest.md, lot B8, slice "Task"; WP 11 §2).

A hedging backtest in a Heston world prices the option on every path at every
rebalancing date: minutes on the production server's two CPUs, past what an
HTTP request should hold. The request creates a task and returns its id at
once; the page polls the task for its progress and its result, and may cancel
it.

One worker runs the tasks one at a time, in the order they came: the server
has two CPUs, shared with the pricers, and two heavy runs side by side would
only take twice as long each. The queue is bounded (a public site), and a
finished task is kept for TASK_TTL seconds for the page to read, then dropped.
The registry lives in the process: the API runs as one uvicorn process, and a
restart loses the tasks in flight, which the page reports as an unknown task.
"""

from __future__ import annotations

import threading
import time
import uuid
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from typing import Any, Callable, Optional

from ..logging_utils import get_logger

_log = get_logger()

#: Tasks waiting or running at once; one more is refused (HTTP 429).
MAX_PENDING = 4
#: How long a finished task stays readable, in seconds.
TASK_TTL = 3600.0


class Cancelled(Exception):
    """Raised inside a run when its task was cancelled."""


class QueueFull(Exception):
    """MAX_PENDING tasks are already waiting or running."""


@dataclass
class Task:
    id: str
    kind: str
    status: str = "queued"  # queued | running | done | failed | cancelled
    progress: float = 0.0
    step: str = ""
    result: Any = None
    error: Optional[str] = None
    created: float = field(default_factory=time.time)
    finished: Optional[float] = None
    cancel_requested: bool = False

    def report(self, fraction: float, step: Optional[str] = None) -> bool:
        """Progress from the run, in [0, 1]; returns True to stop (the
        engine's `cancel` protocol)."""
        self.progress = min(max(float(fraction), 0.0), 1.0)
        if step is not None:
            self.step = step
        return self.cancel_requested


_lock = threading.Lock()
_tasks: dict[str, Task] = {}
_executor = ThreadPoolExecutor(max_workers=1, thread_name_prefix="backtest-task")


def _expire(now: float) -> None:
    for tid in [
        t.id for t in _tasks.values() if t.finished and now - t.finished > TASK_TTL
    ]:
        del _tasks[tid]


def _pending() -> int:
    return sum(t.status in ("queued", "running") for t in _tasks.values())


def submit(
    kind: str,
    run: Callable[[Task], Any],
    on_done: Optional[Callable[[Any], None]] = None,
) -> Task:
    """Queue `run(task)`; `on_done(result)` is called on success (the audit
    event). Raises QueueFull."""
    with _lock:
        _expire(time.time())
        if _pending() >= MAX_PENDING:
            raise QueueFull(
                f"{MAX_PENDING} backtests are already queued or running: try again in a few minutes"
            )
        task = Task(id=uuid.uuid4().hex, kind=kind)
        _tasks[task.id] = task

    def work() -> None:
        if task.cancel_requested:
            task.status, task.finished = "cancelled", time.time()
            return
        task.status = "running"
        try:
            result = run(task)
        except Cancelled:
            task.status = "cancelled"
        except Exception as exc:  # noqa: BLE001 -- reported to the page, logged
            task.status, task.error = "failed", str(exc)
            if not getattr(exc, "user_facing", False):
                _log.exception("backtest task %s failed", task.id)
        else:
            task.result, task.status, task.progress = result, "done", 1.0
            if on_done is not None:
                try:
                    on_done(result)
                except Exception:  # noqa: BLE001 -- the result stands
                    _log.exception("backtest task %s: on_done failed", task.id)
        finally:
            task.finished = time.time()

    _executor.submit(work)
    return task


def get(task_id: str) -> Optional[Task]:
    with _lock:
        _expire(time.time())
        return _tasks.get(task_id)


def cancel(task_id: str) -> Optional[Task]:
    """Ask a task to stop: a queued one never starts, a running one stops at
    the engine's next progress report."""
    task = get(task_id)
    if task is not None and task.status in ("queued", "running"):
        task.cancel_requested = True
    return task


def position(task: Task) -> int:
    """How many tasks run or wait before this one (0: it is running)."""
    with _lock:
        ahead = [
            t
            for t in _tasks.values()
            if t.status in ("queued", "running") and t.created < task.created
        ]
    return len(ahead)
