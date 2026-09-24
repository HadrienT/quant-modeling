"""Event transports (WP §5.1) — same shape as `storage.py`: a `Protocol`,
pluggable implementations, one chosen by `QM_AUDIT_SINK`.
"""

from __future__ import annotations

import os
import sys
import threading
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable, List, Protocol

from .envelope import Event
from .metrics import audit_dropped_total, audit_spool_depth


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

    @property
    def directory(self) -> Path:
        return self._dir

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

    def claim(self) -> List[Path]:
        """Hands the spooled files over to a republisher: each `*.jsonl` is
        renamed to `*.jsonl.replay-<id>` under the append lock, so no event can
        be written to a file after it was claimed (the next `publish` opens a
        fresh daily file). Files a previous process claimed but did not finish
        (a crash mid-replay) are claimed again: replaying them twice is safe,
        `event_id` makes the audit sink idempotent (contract §4)."""
        claimed: List[Path] = []
        with self._lock:
            for path in sorted(self._dir.glob("*.jsonl")):
                target = path.with_name(f"{path.name}.replay-{uuid.uuid4().hex[:8]}")
                try:
                    path.rename(target)
                except OSError:
                    continue
                claimed.append(target)
        claimed.extend(
            p for p in sorted(self._dir.glob("*.jsonl.replay-*")) if p not in claimed
        )
        return claimed

    def depth(self) -> int:
        """Events waiting in the spool, pending ones included (`qm_audit_spool_depth`)."""
        total = 0
        for path in self._dir.glob("*.jsonl*"):
            try:
                with path.open("rb") as fh:
                    total += sum(1 for _ in fh)
            except OSError:
                continue
        return total

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


# ── Kafka (lot 18b) ──────────────────────────────────────────────────────────

#: Event type → topic (contract §2). An event whose type is not here is a
#: producer bug: it stays in the spool rather than being guessed onto a topic.
_TOPIC_BY_TYPE = {
    "pricing.valuation": "qm.audit.valuation.v1",
    "data.fallback": "qm.dataquality.fallback.v1",
    "http.access": "qm.http.access.v1",
    "assistant.chat": "qm.assistant.chat.v1",
}
_AUTH_TOPIC = "qm.audit.auth.v1"


class UnroutableEvent(ValueError):
    """An event type with no topic in the contract."""


def route(event: Event) -> tuple[str, str]:
    """(topic, key) of an event, as the contract (§2) defines them.

    The key decides the partition, and Kafka only orders within a partition:
    `username` keeps one user's actions in order (failed logins, then the
    success), `anon:<ip hash>` does the same for an anonymous caller.
    """
    if event.type.startswith("auth."):
        topic = _AUTH_TOPIC
    else:
        try:
            topic = _TOPIC_BY_TYPE[event.type]
        except KeyError:
            raise UnroutableEvent(f"no topic for event type {event.type!r}") from None

    payload = event.payload
    if topic == "qm.dataquality.fallback.v1":
        key = str(payload.get("kind", ""))
    elif topic == "qm.http.access.v1":
        key = str(payload.get("route", ""))
    elif event.username:
        key = event.username
    else:
        key = f"anon:{payload.get('ip_hash') or 'unknown'}"
    return topic, key


def _kafka_config(bootstrap: str) -> dict[str, Any]:
    """Producer settings of the contract (§4), each for a reason:

    - `acks=all` + `enable.idempotence`: a write is acknowledged only once it
      is durable, and a retry after a lost acknowledgement cannot duplicate it;
    - `linger.ms=20`: waits up to 20 ms to batch messages, which also bounds
      what a crash of the API can lose (ADR-006);
    - `zstd`: the JSON envelopes compress well;
    - `message.timeout.ms`: how long a message may wait for a broker that is
      down before its delivery fails — at which point it goes to the spool.
    """
    return {
        "bootstrap.servers": bootstrap,
        "client.id": "quant-modeling-api",
        "acks": "all",
        "enable.idempotence": True,
        "linger.ms": 20,
        "compression.type": "zstd",
        "message.timeout.ms": 30_000,
        # librdkafka logs every failed connection attempt; one line per
        # reconnection back-off is enough to see a broker is down.
        "reconnect.backoff.max.ms": 10_000,
    }


class KafkaSink:
    """The real transport of lot 18b: a `confluent-kafka` producer to the
    `quant-platform` broker, with the spool as its safety net.

    - `publish` hands the event to librdkafka's in-memory queue and returns:
      sending is asynchronous, so a slow or absent broker never slows a request
      (platform principle 6).
    - A background thread calls `poll()`, which runs the delivery callbacks. A
      failed delivery (broker down past `message.timeout.ms`, topic missing…)
      writes the event to the spool instead: nothing is lost silently.
    - The same thread republishes the spool once the broker answers again:
      each file is claimed, its events produced, and the file deleted once
      every one of them was either acknowledged or spooled again.
    - `close` flushes what is queued; what cannot be sent in time is spooled.
    """

    def __init__(
        self,
        bootstrap: str,
        spool: SpoolSink,
        *,
        producer_factory: Callable[[dict[str, Any]], Any] | None = None,
        republish_interval_s: float = 30.0,
    ) -> None:
        if producer_factory is None:
            from confluent_kafka import Producer

            producer_factory = Producer
        self._spool = spool
        self._producer = producer_factory(_kafka_config(bootstrap))
        self._republish_interval_s = republish_interval_s
        self._stop = threading.Event()
        # Claimed spool files whose events are still awaiting their delivery
        # callbacks: never claimed a second time while in flight.
        self._inflight: set[Path] = set()
        self._inflight_lock = threading.Lock()
        self._failing = False
        self._spooled_count = 0
        self._state_lock = threading.Lock()
        self._thread = threading.Thread(
            target=self._run, name="audit-kafka", daemon=True
        )
        self._thread.start()

    # -- sending ----------------------------------------------------------

    def publish(self, event: Event) -> None:
        self._produce(event, on_done=None)

    def _produce(self, event: Event, on_done: Callable[[], None] | None) -> None:
        def delivered(err, _msg) -> None:
            if err is None:
                self._delivering_again()
            else:
                self._spool.publish(event)
                self._spooled(err)
            if on_done is not None:
                on_done()

        try:
            topic, key = route(event)
        except UnroutableEvent as exc:
            print(f"audit: {exc}; spooled", file=sys.stderr)
            self._spool.publish(event)
            if on_done is not None:
                on_done()
            return
        try:
            value = event.model_dump_json().encode("utf-8")
            self._producer.produce(
                topic, value=value, key=key.encode("utf-8"), on_delivery=delivered
            )
        # BufferError (librdkafka's queue is full), KafkaException…
        except Exception as exc:  # noqa: BLE001
            self._spool.publish(event)
            self._spooled(exc)
            if on_done is not None:
                on_done()

    # One stderr line when Kafka stops taking events and one when it takes them
    # again — not one per event, which would flood the logs during an outage.

    def _spooled(self, reason: object) -> None:
        with self._state_lock:
            first = not self._failing
            self._failing = True
            self._spooled_count += 1
        if first:
            print(
                f"audit: Kafka is not taking events ({reason}); spooling them "
                "until it does",
                file=sys.stderr,
            )

    def _delivering_again(self) -> None:
        if not self._failing:
            return
        with self._state_lock:
            if not self._failing:
                return
            self._failing, n = False, self._spooled_count
            self._spooled_count = 0
        print(
            f"audit: Kafka is taking events again; {n} were spooled meanwhile "
            "and are republished from the spool",
            file=sys.stderr,
        )

    # -- background: callbacks and spool republication --------------------

    def _run(self) -> None:
        next_republish = time.monotonic()
        while not self._stop.is_set():
            self._producer.poll(0.5)
            if time.monotonic() >= next_republish:
                try:
                    self.republish_spool()
                except Exception as exc:  # noqa: BLE001 — the thread must survive
                    print(f"audit: spool republication failed: {exc}", file=sys.stderr)
                next_republish = time.monotonic() + self._republish_interval_s

    def broker_reachable(self, timeout_s: float = 5.0) -> bool:
        try:
            self._producer.list_topics(timeout=timeout_s)
            return True
        except Exception:  # noqa: BLE001
            return False

    def republish_spool(self) -> int:
        """Sends the spooled events to Kafka if the broker answers; returns how
        many were queued. Called by the background thread; public so that tests
        and the resilience check can trigger it."""
        audit_spool_depth.set(self._spool.depth())
        if not any(self._spool.directory.glob("*.jsonl*")):
            return 0
        if not self.broker_reachable():
            return 0
        queued = 0
        for path in self._spool.claim():
            with self._inflight_lock:
                if path in self._inflight:
                    continue
                self._inflight.add(path)
            queued += self._republish_file(path)
        audit_spool_depth.set(self._spool.depth())
        return queued

    def _republish_file(self, path: Path) -> int:
        try:
            lines = path.read_text(encoding="utf-8").splitlines()
        except OSError as exc:
            print(f"audit: cannot read spool file {path}: {exc}", file=sys.stderr)
            with self._inflight_lock:
                self._inflight.discard(path)  # retried at the next pass
            return 0
        events: List[Event] = []
        for line in lines:
            if not line.strip():
                continue
            try:
                events.append(Event.model_validate_json(line))
            except Exception as exc:  # noqa: BLE001 — a corrupt line is dropped, loudly
                audit_dropped_total.inc()
                print(
                    f"qm_audit_dropped_total: unreadable spool line in {path.name}: {exc}",
                    file=sys.stderr,
                )
        if not events:
            self._finish_file(path)
            return 0

        remaining = len(events)
        lock = threading.Lock()

        def one_done() -> None:
            nonlocal remaining
            with lock:
                remaining -= 1
                last = remaining == 0
            if last:
                self._finish_file(path)

        for event in events:
            self._produce(event, on_done=one_done)
        return len(events)

    def _finish_file(self, path: Path) -> None:
        # Every event of the file is now either acknowledged by Kafka or back
        # in a fresh spool file: the claimed copy is no longer needed.
        path.unlink(missing_ok=True)
        with self._inflight_lock:
            self._inflight.discard(path)

    def flush(self, timeout_s: float = 10.0) -> int:
        """Waits for queued messages; returns how many are still undelivered."""
        return self._producer.flush(timeout_s)

    def close(self) -> None:
        self._stop.set()
        self._thread.join(timeout=5)
        left = self._producer.flush(10)
        if left:
            # Purging fires the delivery callback of every message still queued
            # with an error, which spools it: nothing is lost at shutdown.
            try:
                self._producer.purge()
            except Exception:  # noqa: BLE001
                pass
            self._producer.flush(0)


_INSTANCE: EventSink | None = None


def _spool_dir() -> str:
    return os.getenv(
        "QM_AUDIT_SPOOL_DIR",
        str(Path(os.getenv("LOG_DIR", "/tmp/quantmodeling")) / "spool"),
    )


def get_sink() -> EventSink:
    global _INSTANCE
    if _INSTANCE is not None:
        return _INSTANCE
    backend = os.getenv("QM_AUDIT_SINK", "spool").lower()
    spool = SpoolSink(_spool_dir())
    if backend == "kafka":
        bootstrap = os.getenv("QM_KAFKA_BOOTSTRAP", "kafka:9092")
        try:
            _INSTANCE = KafkaSink(bootstrap, spool)
        except (
            Exception
        ) as exc:  # noqa: BLE001 — a bad config must not take the API down
            print(
                f"audit: Kafka producer unavailable ({exc}); falling back to the spool",
                file=sys.stderr,
            )
            _INSTANCE = spool
    else:
        _INSTANCE = spool
    return _INSTANCE


def close_sink() -> None:
    """Flushes and releases the transport (FastAPI shutdown)."""
    global _INSTANCE
    if _INSTANCE is not None:
        _INSTANCE.close()
        _INSTANCE = None


def set_sink_for_testing(sink: EventSink) -> None:
    global _INSTANCE
    _INSTANCE = sink


def reset_sink_for_testing() -> None:
    global _INSTANCE
    _INSTANCE = None
