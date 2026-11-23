"""Give the GPU memory pools back once the API has been idle (issue #350).

Each card's memory pool keeps what a request freed, for the next one, up to
QM_GPU_POOL_RETAIN_MB (read by the C++ library; production sets 6144). A
request that has to allocate its memory again pays for it: after the
local-vol adjoint (4.7 GB of trails for the phoenix on the page's grid), an
SLV pricing ran 17 % slower with a 256 MiB pool, as fast as alone with a
larger one (qm_gpu_risk_bench 65536, one V100, 2026-11-23).

A large pool must not keep that memory from the card's other users (the
development tests, the embedding servers) while nobody prices. So the
pricing routes mark their computations (`busy()`), and a background thread
trims the pools to QM_GPU_POOL_IDLE_KEEP_MB (256) once no computation has
run for QM_GPU_POOL_IDLE_S (30) seconds, once per quiet period. During a
burst of requests the memory stays where it is reused.
"""

from __future__ import annotations

import logging
import os
import threading
import time
from contextlib import contextmanager
from typing import Iterator, Optional

from .logging_utils import LOGGER_NAME

IDLE_S = float(os.environ.get("QM_GPU_POOL_IDLE_S") or 30)
KEEP_BYTES = int(os.environ.get("QM_GPU_POOL_IDLE_KEEP_MB") or 256) << 20

_lock = threading.Lock()
_running = 0
_last_end = time.monotonic()
_trimmed = True  # nothing to give back before the first computation
_stop = threading.Event()
_thread: Optional[threading.Thread] = None


@contextmanager
def busy() -> Iterator[None]:
    """Marks a computation that may use the GPU: no trim while it runs, and
    the idle period starts when the last one ends."""
    global _running, _trimmed, _last_end
    with _lock:
        _running += 1
        _trimmed = False
    try:
        yield
    finally:
        with _lock:
            _running -= 1
            _last_end = time.monotonic()


def trim_if_idle(now: Optional[float] = None, trim=None) -> Optional[int]:
    """Trims the pools if no computation has run for IDLE_S, once per quiet
    period; returns the bytes released, None when it did not trim."""
    global _trimmed
    now = time.monotonic() if now is None else now
    with _lock:
        if _running or _trimmed or now - _last_end < IDLE_S:
            return None
        _trimmed = True
    if trim is None:
        import quantmodeling as qm

        trim = qm.gpu_trim_idle_pools
    return int(trim(KEEP_BYTES))


def _watch() -> None:
    log = logging.getLogger(LOGGER_NAME)
    while not _stop.wait(max(IDLE_S / 3.0, 1.0)):
        try:
            released = trim_if_idle()
        except Exception:  # noqa: BLE001 -- never let the watcher die
            log.exception("gpu pool trim failed")
            continue
        if released:
            log.info(
                "gpu pools trimmed after %.0f s idle: %d MiB released",
                IDLE_S,
                released >> 20,
            )


def start() -> None:
    """Starts the watcher, on a server whose library has a usable GPU."""
    global _thread
    import quantmodeling as qm

    if _thread is not None or not qm.gpu_devices():
        return
    _stop.clear()
    _thread = threading.Thread(target=_watch, name="gpu-pool-trim", daemon=True)
    _thread.start()


def stop() -> None:
    global _thread
    _stop.set()
    _thread = None
