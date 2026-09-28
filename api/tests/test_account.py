"""Account export and deletion (GDPR art. 15, 17 and 20), and what a token is
worth once its account is gone.

Run from the repo root:  pytest api/tests/test_account.py
"""

from __future__ import annotations

import os

os.environ.setdefault("JWT_SECRET", "test-only-secret-not-for-deployment")

import pytest
from fastapi.testclient import TestClient

from api.app import storage
from api.app.audit.sinks import (
    InMemorySink,
    reset_sink_for_testing,
    set_sink_for_testing,
)
from api.app.main import app


@pytest.fixture
def sink():
    s = InMemorySink()
    set_sink_for_testing(s)
    yield s
    reset_sink_for_testing()


@pytest.fixture
def client(tmp_path, monkeypatch, sink):
    monkeypatch.setattr(storage, "_INSTANCE", storage.LocalJsonStorage(tmp_path))
    return TestClient(app)


def _register(client, name="alice", password="correct-horse") -> dict:
    r = client.post("/api/auth/register", json={"username": name, "password": password})
    assert r.status_code == 201, r.text
    return {"Authorization": f"Bearer {r.json()['token']}"}


def _portfolio(client, auth, name="Book") -> str:
    r = client.post("/api/portfolios", params={"name": name}, headers=auth)
    assert r.status_code in (200, 201), r.text
    return r.json()["id"]


def test_export_holds_the_account_and_its_portfolios_but_no_password(client):
    auth = _register(client)
    pid = _portfolio(client, auth)
    r = client.get("/api/auth/me/export", headers=auth)
    assert r.status_code == 200
    body = r.json()
    assert body["account"]["username"] == "alice"
    assert [p["id"] for p in body["portfolios"]] == [pid]
    assert "hashed_password" not in r.text and "correct-horse" not in r.text
    assert body["not_included"]


def test_deletion_removes_the_record_and_every_portfolio_file(client, tmp_path):
    auth = _register(client)
    _portfolio(client, auth, "A")
    _portfolio(client, auth, "B")
    assert list((tmp_path / "portfolios").iterdir())

    assert client.delete("/api/auth/me", headers=auth).status_code == 204

    assert "alice" not in (tmp_path / "auth" / "users.json").read_text()
    # no file, and no directory still named after the owner
    assert not (tmp_path / "portfolios").exists() or not any(
        (tmp_path / "portfolios").iterdir()
    )


def test_a_token_dies_with_its_account(client):
    auth = _register(client)
    client.delete("/api/auth/me", headers=auth)
    assert client.get("/api/auth/me", headers=auth).status_code == 401
    assert client.get("/api/portfolios", headers=auth).status_code == 401


def test_an_old_token_does_not_open_an_account_registered_later_under_its_name(
    client, monkeypatch
):
    from api.app import auth as auth_module

    old = _register(client, "bob")
    client.delete("/api/auth/me", headers=old)
    # the new account is younger than the old token
    real_now = auth_module.datetime

    class Later(real_now):
        @classmethod
        def now(cls, tz=None):
            return real_now.now(tz).replace(year=real_now.now(tz).year + 1)

    monkeypatch.setattr(auth_module, "datetime", Later)
    new = _register(client, "bob")
    monkeypatch.setattr(auth_module, "datetime", real_now)

    assert client.get("/api/auth/me", headers=old).status_code == 401
    assert client.get("/api/auth/me", headers=new).status_code == 200


def test_deletion_is_audited_without_personal_payload(client, sink):
    auth = _register(client)
    client.delete("/api/auth/me", headers=auth)
    deleted = [e for e in sink.events if e.type == "auth.account_deleted"]
    assert len(deleted) == 1
    assert deleted[0].username == "alice"
    assert set(deleted[0].payload) <= {"outcome", "ip_hash"}


def test_both_routes_need_a_session(client):
    assert client.get("/api/auth/me/export").status_code == 401
    assert client.delete("/api/auth/me").status_code == 401
