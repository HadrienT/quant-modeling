"""Sign in with Google: state/PKCE handling, ID-token checks, account rules.

Google itself is faked: the ID token is signed with a locally generated RSA key
whose public half stands in for Google's JWKS. Nothing here touches the network.
Run from the repo root:  pytest api/tests/test_google_oauth.py
"""

from __future__ import annotations

import os
import time
from urllib.parse import parse_qs, urlparse

os.environ.setdefault("JWT_SECRET", "test-only-secret-not-for-deployment")

import pytest
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import rsa
from fastapi import HTTPException
from fastapi.testclient import TestClient
from jose import jwt
from jose.backends.cryptography_backend import CryptographyRSAKey
from jose.constants import ALGORITHMS

from api.app import auth, google_oauth, storage
from api.app.main import app

CLIENT_ID = "test-client.apps.googleusercontent.com"


@pytest.fixture(autouse=True)
def _env(tmp_path, monkeypatch):
    monkeypatch.setenv("GOOGLE_CLIENT_ID", CLIENT_ID)
    monkeypatch.setenv("GOOGLE_CLIENT_SECRET", "test-secret")
    monkeypatch.setenv("QM_PUBLIC_URL", "https://qm.example.com")
    monkeypatch.setattr(storage, "_INSTANCE", storage.LocalJsonStorage(tmp_path))
    monkeypatch.setattr(google_oauth, "_jwks_cache", (0.0, {}))


@pytest.fixture(scope="module")
def rsa_key():
    private = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    pem = private.private_bytes(
        serialization.Encoding.PEM,
        serialization.PrivateFormat.PKCS8,
        serialization.NoEncryption(),
    )
    public = CryptographyRSAKey(private.public_key(), ALGORITHMS.RS256).to_dict()
    public.update(kid="k1", use="sig")
    return pem, {"keys": [public]}


def _id_token(pem, **over):
    claims = {
        "iss": "https://accounts.google.com",
        "aud": CLIENT_ID,
        "sub": "1234567890",
        "email": "Alice@Example.com",
        "email_verified": True,
        "nonce": "N",
        "iat": int(time.time()),
        "exp": int(time.time()) + 300,
    }
    claims.update(over)
    return jwt.encode(claims, pem, algorithm="RS256", headers={"kid": "k1"})


@pytest.fixture
def fake_google(monkeypatch, rsa_key):
    """Patch Google's two endpoints; returns a setter for the ID-token claims."""
    pem, jwks = rsa_key
    state = {"claims": {}}
    monkeypatch.setattr(google_oauth, "_google_keys", lambda force=False: jwks)

    def exchange(code, verifier):
        assert code == "good-code"
        return {"id_token": _id_token(pem, **state["claims"]), "access_token": "at"}

    monkeypatch.setattr(google_oauth, "_exchange_code", exchange)
    return state


def _begin(client):
    """Hit /login; return (state param sent to Google, nonce, cookie jar)."""
    r = client.get("/api/auth/google/login", follow_redirects=False)
    assert r.status_code == 302
    q = parse_qs(urlparse(r.headers["location"]).query)
    assert q["code_challenge_method"] == ["S256"]
    assert q["redirect_uri"] == ["https://qm.example.com/api/auth/google/callback"]
    cookie = google_oauth._read_state_cookie(r.cookies[google_oauth.STATE_COOKIE])
    return q["state"][0], cookie["nonce"]


def test_disabled_without_credentials(monkeypatch):
    monkeypatch.delenv("GOOGLE_CLIENT_SECRET")
    c = TestClient(app, base_url="https://qm.example.com")
    assert c.get("/api/auth/providers").json() == {"google": False}
    assert c.get("/api/auth/google/login", follow_redirects=False).status_code == 404


def test_login_sets_httponly_secure_state_cookie():
    c = TestClient(app, base_url="https://qm.example.com")
    r = c.get("/api/auth/google/login", follow_redirects=False)
    header = r.headers["set-cookie"].lower()
    assert "httponly" in header and "secure" in header and "samesite=lax" in header
    assert c.get("/api/auth/providers").json() == {"google": True}


def test_full_flow_creates_google_account(fake_google):
    c = TestClient(app, base_url="https://qm.example.com")
    state, nonce = _begin(c)
    fake_google["claims"]["nonce"] = nonce
    r = c.get(
        "/api/auth/google/callback",
        params={"code": "good-code", "state": state},
        follow_redirects=False,
    )
    assert r.status_code == 302
    loc = r.headers["location"]
    assert loc.startswith("/#qm_auth=")
    token = loc.split("qm_auth=")[1]
    me = c.get("/api/auth/me", headers={"Authorization": f"Bearer {token}"}).json()
    assert me["username"] == "google:1234567890"
    assert me["email"] == "alice@example.com"
    assert me["provider"] == "google"
    assert me["created_at"]


def test_second_login_reuses_account(fake_google):
    c = TestClient(app, base_url="https://qm.example.com")
    for _ in range(2):
        state, nonce = _begin(c)
        fake_google["claims"]["nonce"] = nonce
        c.get(
            "/api/auth/google/callback",
            params={"code": "good-code", "state": state},
            follow_redirects=False,
        )
    assert list(auth._load_users()) == ["google:1234567890"]


@pytest.mark.parametrize(
    "claims",
    [
        {"aud": "someone-else"},
        {"iss": "https://evil.example.com"},
        {"exp": int(time.time()) - 10},
        {"email_verified": False},
        {"nonce": "wrong"},
    ],
)
def test_bad_id_token_is_rejected(fake_google, claims):
    c = TestClient(app, base_url="https://qm.example.com")
    state, nonce = _begin(c)
    fake_google["claims"] = {"nonce": nonce, **claims}
    r = c.get(
        "/api/auth/google/callback",
        params={"code": "good-code", "state": state},
        follow_redirects=False,
    )
    assert r.headers["location"].startswith("/#qm_auth_error=")
    assert auth._load_users() == {}


def test_state_mismatch_and_missing_cookie_are_rejected(fake_google):
    c = TestClient(app, base_url="https://qm.example.com")
    _begin(c)
    r = c.get(
        "/api/auth/google/callback",
        params={"code": "good-code", "state": "forged"},
        follow_redirects=False,
    )
    assert r.headers["location"].startswith("/#qm_auth_error=")
    fresh = TestClient(
        app, base_url="https://qm.example.com"
    )  # no state cookie at all (e.g. cross-site forgery)
    r = fresh.get(
        "/api/auth/google/callback",
        params={"code": "good-code", "state": "x"},
        follow_redirects=False,
    )
    assert r.headers["location"].startswith("/#qm_auth_error=")


def test_user_cancelling_at_google_redirects_with_error():
    r = TestClient(app, base_url="https://qm.example.com").get(
        "/api/auth/google/callback",
        params={"error": "access_denied"},
        follow_redirects=False,
    )
    assert r.headers["location"] == "/#qm_auth_error=access_denied"


def test_session_jwt_cannot_be_used_as_state_cookie():
    token = auth._create_token("google:1")
    with pytest.raises(HTTPException):
        google_oauth._read_state_cookie(token)


def test_password_users_cannot_claim_provider_namespace_or_log_in_to_google_accounts():
    with pytest.raises(HTTPException) as e:
        auth.register_user("google:1234567890", "password123")
    assert e.value.status_code == 400
    auth.login_google("1234567890", "alice@example.com")
    with pytest.raises(HTTPException) as e:
        auth.login_user("google:1234567890", "")  # empty hash must never match
    assert e.value.status_code == 401
