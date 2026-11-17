"""Read access to the audit database of `quant-platform` (contract §3).

The API never writes there — only `audit-sink` does (platform principle 2) —
and reads with the `audit_reader` role, for two purposes: the replay endpoint
(blueprint WP 18e), and the journal of the backtests tried on a universe and a
period, which the deflated Sharpe ratio counts (WP 24, lot B8). Same idiom as the sinks: a `Protocol`, the Postgres
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

    def backtest_requests(
        self, start: str, end: str, tickers: list[str], universe: str, limit: int
    ) -> list[dict[str, Any]]: ...


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

    def backtest_requests(
        self, start: str, end: str, tickers: list[str], universe: str, limit: int
    ) -> list[dict[str, Any]]:
        """The requests of the strategy and validation backtests recorded on
        this period and universe (the same tickers in any order, or the same
        index universe), most recent first: {"kind", "request"}."""
        import psycopg

        try:
            with psycopg.connect(self._conninfo, connect_timeout=5) as conn:
                rows = conn.execute(
                    "SELECT payload->>'kind', payload->'request' FROM audit.events "
                    "WHERE type = 'backtest.run' "
                    "AND payload->>'kind' IN ('strategy', 'validation') "
                    "AND payload->'request'->>'start' = %s "
                    "AND payload->'request'->>'end' = %s "
                    "AND COALESCE(payload->'request'->>'universe', 'chosen') = %s "
                    "AND (payload->'request'->'tickers') @> %s::jsonb "
                    "AND %s::jsonb @> (payload->'request'->'tickers') "
                    "ORDER BY occurred_at DESC LIMIT %s",
                    (
                        start,
                        end,
                        universe,
                        json.dumps(tickers),
                        json.dumps(tickers),
                        limit,
                    ),
                ).fetchall()
        except psycopg.Error as exc:
            raise AuditStoreUnavailable(f"audit database unreachable: {exc}") from exc
        return [
            {"kind": kind, "request": json.loads(req) if isinstance(req, str) else req}
            for kind, req in rows
        ]


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
