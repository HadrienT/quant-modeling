"""The pages an administrator hides from visitors (`site_pages.py`)."""

from __future__ import annotations

import os
import re
from pathlib import Path

os.environ.setdefault("JWT_SECRET", "test-only-secret-not-for-deployment")

import pytest  # noqa: E402
from fastapi.testclient import TestClient  # noqa: E402

from api.app import auth, site_pages  # noqa: E402
from api.app.audit.sinks import (  # noqa: E402
    InMemorySink,
    reset_sink_for_testing,
    set_sink_for_testing,
)
from api.app.main import app  # noqa: E402

PAGES_TS = Path(__file__).resolve().parents[2] / "web/src/shared/config/pages.ts"


@pytest.fixture
def sink():
    s = InMemorySink()
    set_sink_for_testing(s)
    yield s
    reset_sink_for_testing()


@pytest.fixture
def client(sink, monkeypatch):
    monkeypatch.setenv("QM_ADMIN_USERS", "boss")
    return TestClient(app)


def _bearer(username: str) -> dict:
    """A token is only honoured for an account that exists: create it."""
    users = auth._load_users()
    users[username] = auth.UserRecord(username=username, hashed_password="")
    auth._save_users(users)
    return {"Authorization": f"Bearer {auth._create_token(username)}"}


def test_the_hideable_pages_are_the_navigation_of_the_front():
    paths = re.findall(r'path:\s*"([^"]+)"', PAGES_TS.read_text())
    assert tuple(paths) == site_pages.HIDEABLE_PAGES


def test_every_page_is_shown_until_an_admin_hides_one(client):
    r = client.get("/api/site/pages")
    assert r.status_code == 200 and r.json() == {"hidden": []}


def test_only_an_administrator_changes_the_pages(client):
    body = {"hidden": ["/xva"]}
    assert client.put("/api/admin/pages", json=body).status_code == 401
    r = client.put("/api/admin/pages", json=body, headers=_bearer("someone"))
    assert r.status_code == 403
    assert client.get("/api/admin/pages", headers=_bearer("someone")).status_code == 403
    assert client.get("/api/site/pages").json() == {"hidden": []}


def test_an_admin_hides_pages_and_every_visitor_sees_it(client, sink):
    r = client.put(
        "/api/admin/pages",
        json={"hidden": ["/xva", "/price", "/xva"]},
        headers=_bearer("boss"),
    )
    assert r.status_code == 200
    # navigation order, no duplicate, and who did it
    assert r.json()["hidden"] == ["/price", "/xva"]
    assert r.json()["updated_by"] == "boss"
    assert client.get("/api/site/pages").json() == {"hidden": ["/price", "/xva"]}
    events = [e for e in sink.events if e.type == "admin.site_pages"]
    assert len(events) == 1
    assert events[0].payload == {"hidden": ["/price", "/xva"]}
    assert events[0].username == "boss"

    client.put("/api/admin/pages", json={"hidden": []}, headers=_bearer("boss"))
    assert client.get("/api/site/pages").json() == {"hidden": []}


def test_an_unknown_page_is_refused(client):
    r = client.put(
        "/api/admin/pages",
        json={"hidden": ["/profile", "/nope"]},
        headers=_bearer("boss"),
    )
    assert r.status_code == 422 and "/nope" in r.json()["message"]
    assert client.get("/api/site/pages").json() == {"hidden": []}


def test_me_says_who_is_an_administrator(client):
    assert client.get("/api/auth/me", headers=_bearer("boss")).json()["is_admin"]
    me = client.get("/api/auth/me", headers=_bearer("someone")).json()
    assert me["is_admin"] is False
