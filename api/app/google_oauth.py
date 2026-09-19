"""Sign in with Google — OpenID Connect authorization-code flow with PKCE.

The API is the OAuth client (confidential: it holds GOOGLE_CLIENT_SECRET), the
browser only navigates:

    /api/auth/google/login     -> 302 to Google, sets a short-lived state cookie
    /api/auth/google/callback  -> Google redirects back with ?code&state; we
                                  exchange the code, verify the ID token, then
                                  redirect to the SPA with our own JWT.

No new dependency: `requests` talks to Google, `python-jose` verifies the ID
token against Google's published keys.

Google accounts are stored as `google:<sub>` — `sub` is Google's stable account
id (an email can change or be recycled). Password usernames cannot contain ':'
(see auth.register_user), so a password account can never collide with, or
pre-claim, a Google one.
"""

from __future__ import annotations

import base64
import hashlib
import os
import secrets
import time
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from typing import Any, Optional
from urllib.parse import urlencode

import requests
from fastapi import HTTPException
from jose import JWTError, jwt

from .auth import JWT_ALGORITHM, JWT_SECRET

AUTH_URL = "https://accounts.google.com/o/oauth2/v2/auth"
TOKEN_URL = "https://oauth2.googleapis.com/token"
JWKS_URL = "https://www.googleapis.com/oauth2/v3/certs"
ISSUERS = ("https://accounts.google.com", "accounts.google.com")

STATE_COOKIE = "qm_oauth_state"
STATE_TTL_S = 600
_HTTP_TIMEOUT_S = 8
_JWKS_TTL_S = 3600


@dataclass(frozen=True)
class GoogleProfile:
    sub: str
    email: str


def _client_id() -> str:
    return os.getenv("GOOGLE_CLIENT_ID", "").strip()


def _client_secret() -> str:
    return os.getenv("GOOGLE_CLIENT_SECRET", "").strip()


def is_enabled() -> bool:
    return bool(_client_id() and _client_secret())


def public_url() -> str:
    """Origin users reach the app on; Google must be told the same one."""
    return os.getenv("QM_PUBLIC_URL", "http://localhost:5180").rstrip("/")


def redirect_uri() -> str:
    return f"{public_url()}/api/auth/google/callback"


def cookie_secure() -> bool:
    return public_url().startswith("https://")


def _require_enabled() -> None:
    if not is_enabled():
        raise HTTPException(status_code=404, detail="Google sign-in is not configured")


# ── State cookie (CSRF + PKCE verifier + nonce, signed, 10 min) ──────────────


def _pkce_challenge(verifier: str) -> str:
    digest = hashlib.sha256(verifier.encode()).digest()
    return base64.urlsafe_b64encode(digest).rstrip(b"=").decode()


def start_login() -> tuple[str, str]:
    """Return (google_authorization_url, signed_state_cookie_value)."""
    _require_enabled()
    state = secrets.token_urlsafe(24)
    verifier = secrets.token_urlsafe(48)
    nonce = secrets.token_urlsafe(16)
    cookie = jwt.encode(
        {
            "typ": "oauth_state",
            "state": state,
            "verifier": verifier,
            "nonce": nonce,
            "exp": datetime.now(timezone.utc) + timedelta(seconds=STATE_TTL_S),
        },
        JWT_SECRET,
        algorithm=JWT_ALGORITHM,
    )
    url = (
        AUTH_URL
        + "?"
        + urlencode(
            {
                "client_id": _client_id(),
                "redirect_uri": redirect_uri(),
                "response_type": "code",
                "scope": "openid email",
                "state": state,
                "nonce": nonce,
                "code_challenge": _pkce_challenge(verifier),
                "code_challenge_method": "S256",
                "prompt": "select_account",
            }
        )
    )
    return url, cookie


def _read_state_cookie(cookie: Optional[str]) -> dict[str, Any]:
    if not cookie:
        raise HTTPException(status_code=400, detail="Missing OAuth state")
    try:
        claims = jwt.decode(cookie, JWT_SECRET, algorithms=[JWT_ALGORITHM])
    except JWTError:
        raise HTTPException(status_code=400, detail="Invalid or expired OAuth state")
    if claims.get("typ") != "oauth_state":
        raise HTTPException(status_code=400, detail="Invalid OAuth state")
    return claims


# ── Google round-trip ────────────────────────────────────────────────────────

_jwks_cache: tuple[float, dict[str, Any]] = (0.0, {})


def _google_keys(force: bool = False) -> dict[str, Any]:
    global _jwks_cache
    fetched_at, keys = _jwks_cache
    if force or not keys or time.monotonic() - fetched_at > _JWKS_TTL_S:
        resp = requests.get(JWKS_URL, timeout=_HTTP_TIMEOUT_S)
        resp.raise_for_status()
        keys = resp.json()
        _jwks_cache = (time.monotonic(), keys)
    return keys


def _exchange_code(code: str, verifier: str) -> dict[str, Any]:
    try:
        resp = requests.post(
            TOKEN_URL,
            data={
                "code": code,
                "client_id": _client_id(),
                "client_secret": _client_secret(),
                "redirect_uri": redirect_uri(),
                "grant_type": "authorization_code",
                "code_verifier": verifier,
            },
            timeout=_HTTP_TIMEOUT_S,
        )
    except requests.RequestException:
        raise HTTPException(status_code=502, detail="Could not reach Google")
    if resp.status_code != 200:
        raise HTTPException(status_code=400, detail="Google rejected the sign-in code")
    return resp.json()


def _verify_id_token(tokens: dict[str, Any], nonce: str) -> GoogleProfile:
    id_token = tokens.get("id_token")
    if not id_token:
        raise HTTPException(status_code=400, detail="Google returned no ID token")

    def _decode(keys: dict[str, Any]) -> dict[str, Any]:
        return jwt.decode(
            id_token,
            keys,
            algorithms=["RS256"],
            audience=_client_id(),
            access_token=tokens.get("access_token"),
            options={"verify_iss": False},  # two valid spellings, checked below
        )

    try:
        try:
            claims = _decode(_google_keys())
        except JWTError:
            # Google rotates its keys: retry once against a fresh set.
            claims = _decode(_google_keys(force=True))
    except (JWTError, requests.RequestException):
        raise HTTPException(status_code=400, detail="Invalid Google ID token")

    if claims.get("iss") not in ISSUERS or claims.get("nonce") != nonce:
        raise HTTPException(status_code=400, detail="Invalid Google ID token")
    sub = claims.get("sub")
    email = claims.get("email", "")
    if not sub or not email or claims.get("email_verified") is not True:
        raise HTTPException(
            status_code=400, detail="Google account has no verified email"
        )
    return GoogleProfile(sub=str(sub), email=str(email).lower())


def complete_login(
    *, code: str, state: str, state_cookie: Optional[str]
) -> GoogleProfile:
    """Validate the callback and return the verified Google identity."""
    _require_enabled()
    claims = _read_state_cookie(state_cookie)
    if not secrets.compare_digest(str(claims.get("state", "")), state):
        raise HTTPException(status_code=400, detail="OAuth state mismatch")
    tokens = _exchange_code(code, claims["verifier"])
    return _verify_id_token(tokens, claims["nonce"])
