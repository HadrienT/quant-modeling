"""Read access to the audit database of `quant-platform` (contract §3).

The API never writes there — only `audit-sink` does (platform principle 2) —
and reads with the `audit_reader` role, for one purpose: the replay endpoint
(blueprint WP 18e). Same idiom as the sinks: a `Protocol`, the Postgres
implementation, and a setter for tests.

Connection: `QM_AUDIT_DB_DSN` if set, otherwise `QM_AUDIT_DB_HOST` (default
`qm-audit`, over the `dataplatform` network), `QM_AUDIT_DB_PORT` (5432),
`QM_AUDIT_DB_NAME` (`qm_audit`), `QM_AUDIT_DB_USER` (`audit_reader`) and
`QM_AUDIT_DB_PASSWORD` — the `AUDIT_DB_READER_PASSWORD` of quant-platform's
`.env`. Without a password the store is "not configured", and the replay
endpoint answers 503 rather than guessing.
"""

from __future__ import annotations

import json
import os
from typing import Any, Protocol


class AuditStoreUnavailable(RuntimeError):
    """The audit database is not configured or cannot be reached."""


class EventStore(Protocol):
    def get(self, event_id: str) -> dict[str, Any] | None: ...


class PostgresEventStore:
    def __init__(self, conninfo: str) -> None:
        self._conninfo = conninfo

    def get(self, event_id: str) -> dict[str, Any] | None:
        import psycopg
        from psycopg.rows import dict_row

        try:
            with psycopg.connect(
                self._conninfo, connect_timeout=5, row_factory=dict_row
            ) as conn:
                row = conn.execute(
                    "SELECT event_id::text, type, version, occurred_at, request_id, "
                    "trace_id, username, producer, payload "
                    "FROM audit.events WHERE event_id = %s",
                    (event_id,),
                ).fetchone()
        except psycopg.Error as exc:
            raise AuditStoreUnavailable(f"audit database unreachable: {exc}") from exc
        if row is None:
            return None
        row["occurred_at"] = row["occurred_at"].isoformat()
        for col in ("producer", "payload"):
            if isinstance(row[col], str):
                row[col] = json.loads(row[col])
        return row


_STORE: EventStore | None = None


def _conninfo() -> str | None:
    dsn = os.getenv("QM_AUDIT_DB_DSN")
    if dsn:
        return dsn
    password = os.getenv("QM_AUDIT_DB_PASSWORD")
    if not password:
        return None
    from psycopg.conninfo import make_conninfo

    return make_conninfo(
        host=os.getenv("QM_AUDIT_DB_HOST", "qm-audit"),
        port=os.getenv("QM_AUDIT_DB_PORT", "5432"),
        dbname=os.getenv("QM_AUDIT_DB_NAME", "qm_audit"),
        user=os.getenv("QM_AUDIT_DB_USER", "audit_reader"),
        password=password,
    )


def get_store() -> EventStore:
    global _STORE
    if _STORE is None:
        conninfo = _conninfo()
        if conninfo is None:
            raise AuditStoreUnavailable(
                "the audit database is not configured (QM_AUDIT_DB_PASSWORD or "
                "QM_AUDIT_DB_DSN): replay needs read access to quant-platform's qm-audit"
            )
        _STORE = PostgresEventStore(conninfo)
    return _STORE


def set_store_for_testing(store: EventStore | None) -> None:
    global _STORE
    _STORE = store
