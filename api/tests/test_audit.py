"""Audit trail — blueprint WP 18a. Covers the lot's own acceptance criteria:
one test per known fallback path emits `data.fallback` (InMemorySink), no
payload model can carry a password/token/Authorization field, `emit()` never
raises even when the transport is broken, and `/health` returns X-Request-ID.
Run from the repo root:  pytest api/tests/test_audit.py
"""

from __future__ import annotations

import json
import os
import time

os.environ.setdefault("JWT_SECRET", "test-only-secret-not-for-deployment")

import pytest
from fastapi.testclient import TestClient

from api.app import auth, db
from api.app.audit.emit import emit
from api.app.audit.envelope import Event, uuid7
from api.app.audit.fallback import record_fallback
from api.app.audit.ip_hash import hash_ip
from api.app.audit.payloads import (
    AuthEventPayload,
    FallbackKind,
    FallbackPayload,
    HttpAccessPayload,
)
from api.app.audit.sinks import (
    InMemorySink,
    SpoolSink,
    get_sink,
    reset_sink_for_testing,
    set_sink_for_testing,
)
from api.app.main import app
from api.app.routers import backtest
from api.app import vol_surface
from api.app.cache import TTLCache


@pytest.fixture
def inmemory_sink():
    sink = InMemorySink()
    set_sink_for_testing(sink)
    yield sink
    reset_sink_for_testing()


# ── §5.1: "aucun événement ne contient de champ nommé password, token,
#   authorization" — a schema test, not one run against examples. ──────────────


@pytest.mark.parametrize(
    "model", [FallbackPayload, AuthEventPayload, HttpAccessPayload]
)
def test_payload_models_never_declare_a_secret_field(model):
    forbidden = {"password", "token", "authorization"}
    assert not (set(model.model_fields) & forbidden)


def test_uuid7_is_time_ordered_and_versioned():
    a = uuid7()
    assert a[14] == "7"  # version nibble
    time.sleep(0.002)  # UUID7 only orders across milliseconds, not within one
    b = uuid7()
    assert a < b  # lexical order of the hex string follows the embedded timestamp


# ── emit() must never raise, even if the transport is broken. ──────────────


def test_emit_never_raises_when_the_sink_is_broken():
    class BrokenSink:
        def publish(self, event):
            raise RuntimeError("broker is down")

        def close(self):
            pass

    set_sink_for_testing(BrokenSink())
    try:
        emit("data.fallback", FallbackPayload(kind=FallbackKind.STALE_DATA))
    finally:
        reset_sink_for_testing()


def test_emit_populates_the_envelope(inmemory_sink):
    emit(
        "data.fallback",
        FallbackPayload(kind=FallbackKind.STALE_DATA),
        username="hadrien",
    )
    assert len(inmemory_sink.events) == 1
    event = inmemory_sink.events[0]
    assert isinstance(event, Event)
    assert event.type == "data.fallback"
    assert event.username == "hadrien"
    assert event.producer.service == "quant-modeling-api"


# ── SpoolSink: one JSON line per event, under QM_AUDIT_SPOOL_DIR. ──────────────


def test_spool_sink_appends_one_jsonl_line_per_event(tmp_path):
    sink = SpoolSink(tmp_path)
    sink.publish(
        Event.create("data.fallback", FallbackPayload(kind=FallbackKind.STALE_DATA))
    )
    sink.publish(
        Event.create("data.fallback", FallbackPayload(kind=FallbackKind.PROXIED_INPUT))
    )
    files = list(tmp_path.glob("*.jsonl"))
    assert len(files) == 1
    lines = files[0].read_text().splitlines()
    assert len(lines) == 2
    assert json.loads(lines[0])["type"] == "data.fallback"


def test_get_sink_defaults_to_spool(monkeypatch, tmp_path):
    reset_sink_for_testing()
    monkeypatch.delenv("QM_AUDIT_SINK", raising=False)
    monkeypatch.setenv("QM_AUDIT_SPOOL_DIR", str(tmp_path))
    try:
        assert isinstance(get_sink(), SpoolSink)
    finally:
        reset_sink_for_testing()


def test_hash_ip_is_stable_and_does_not_leak_the_address(monkeypatch):
    monkeypatch.setenv("QM_AUDIT_IP_HMAC_SECRET", "a-server-secret")
    h1 = hash_ip("203.0.113.7")
    h2 = hash_ip("203.0.113.7")
    assert h1 == h2
    assert "203.0.113.7" not in h1
    assert hash_ip(None) is None


# ── One test per known fallback path (WP §5.1 lists them explicitly). ──────────


def test_fallback_option_chain_when_no_stored_snapshot(monkeypatch, inmemory_sink):
    monkeypatch.setattr(
        vol_surface.db,
        "options_chain_snapshot",
        lambda ticker: (_ for _ in ()).throw(db.StoreUnavailable("down")),
    )
    monkeypatch.setattr(vol_surface, "_fetch_live_chain_with_expiry", lambda ticker: [])
    monkeypatch.setattr(vol_surface, "_cache_live_chain", lambda ticker, pairs: None)

    vol_surface.fetch_option_chain("SPY")

    kinds = [
        e.payload["kind"] for e in inmemory_sink.events if e.type == "data.fallback"
    ]
    assert FallbackKind.LIVE_YFINANCE_CHAIN.value in kinds


def test_fallback_spot_when_no_stored_price(monkeypatch, inmemory_sink):
    monkeypatch.setattr(
        vol_surface.db,
        "latest_price",
        lambda ticker: (_ for _ in ()).throw(db.StoreUnavailable("down")),
    )
    monkeypatch.setattr(vol_surface, "_get_spot_live", lambda ticker: 123.0)

    price = vol_surface.get_spot("SPY")

    assert price == 123.0
    kinds = [
        e.payload["kind"] for e in inmemory_sink.events if e.type == "data.fallback"
    ]
    assert FallbackKind.LIVE_YFINANCE_SPOT.value in kinds


def test_fallback_dividend_yield_when_no_stored_value(monkeypatch, inmemory_sink):
    monkeypatch.setattr(
        vol_surface.db,
        "latest_dividend_yield",
        lambda ticker: (_ for _ in ()).throw(db.StoreUnavailable("down")),
    )
    monkeypatch.setattr(vol_surface, "_get_dividend_yield_live", lambda ticker: 0.02)

    yld = vol_surface.get_dividend_yield("SPY")

    assert yld == 0.02
    kinds = [
        e.payload["kind"] for e in inmemory_sink.events if e.type == "data.fallback"
    ]
    assert FallbackKind.LIVE_YFINANCE_DIVIDEND.value in kinds


def test_fallback_default_rate_when_no_macro_rate(monkeypatch, inmemory_sink):
    monkeypatch.setattr(
        backtest, "_RF_RATE_CACHE", TTLCache(max_size=64, ttl_seconds=60 * 30)
    )
    monkeypatch.setattr(backtest.db, "fred_latest_value", lambda series_id: None)

    rate = backtest._fetch_risk_free_rate()

    assert rate == 0.04
    kinds = [
        e.payload["kind"] for e in inmemory_sink.events if e.type == "data.fallback"
    ]
    assert FallbackKind.DEFAULT_RATE.value in kinds


def test_record_fallback_is_a_thin_wrapper_around_emit(inmemory_sink):
    record_fallback(FallbackKind.PROXIED_INPUT, detail="neighbour strike", ticker="SPY")
    assert len(inmemory_sink.events) == 1
    assert inmemory_sink.events[0].payload == {
        "kind": "proxied_input",
        "detail": "neighbour strike",
        "context": {"ticker": "SPY"},
    }


# ── Middleware / auth events, end to end through the real app. ─────────────


@pytest.fixture
def client(monkeypatch, tmp_path):
    monkeypatch.setenv("LOG_DIR", str(tmp_path))
    monkeypatch.setenv("QM_AUDIT_SPOOL_DIR", str(tmp_path / "spool"))
    monkeypatch.setenv("QM_STORAGE", "local")
    monkeypatch.setenv("QM_DATA_DIR", str(tmp_path / "data"))
    reset_sink_for_testing()
    sink = InMemorySink()
    set_sink_for_testing(sink)
    try:
        yield TestClient(app), sink
    finally:
        reset_sink_for_testing()


def test_health_returns_x_request_id(client):
    c, _sink = client
    resp = c.get("/health")
    assert resp.status_code == 200
    assert resp.headers.get("X-Request-ID", "").startswith("req_")


def test_health_emits_http_access_event(client):
    c, sink = client
    c.get("/health")
    access = [e for e in sink.events if e.type == "http.access"]
    assert len(access) == 1
    assert access[0].payload["route"] == "/health"
    assert access[0].payload["status_code"] == 200


def test_register_then_login_emit_the_matching_auth_events(client):
    c, sink = client
    username = f"audituser{uuid7()[:8]}"
    r = c.post(
        "/api/auth/register", json={"username": username, "password": "correct-horse"}
    )
    assert r.status_code == 201
    assert any(
        e.type == "auth.register" and e.username == username for e in sink.events
    )

    r = c.post(
        "/api/auth/login", json={"username": username, "password": "correct-horse"}
    )
    assert r.status_code == 200
    assert any(
        e.type == "auth.login_ok" and e.username == username for e in sink.events
    )

    r = c.post("/api/auth/login", json={"username": username, "password": "wrong"})
    assert r.status_code == 401
    assert any(
        e.type == "auth.login_failed" and e.username == username for e in sink.events
    )


def test_bad_bearer_token_emits_token_invalid(client):
    c, sink = client
    c.get("/api/auth/me", headers={"Authorization": "Bearer not-a-real-token"})
    assert any(e.type == "auth.token_invalid" for e in sink.events)


def test_no_authorization_header_does_not_emit_token_invalid(client):
    c, sink = client
    c.get("/health")
    assert not any(e.type == "auth.token_invalid" for e in sink.events)
