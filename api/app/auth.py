"""Lightweight JWT auth — blueprint WP 04.

Users live in the pluggable Storage backend (local JSON by default, no GCP
needed). JWT_SECRET has no default: the app refuses to start without a real one
(a default secret is worse than a missing one — it goes unnoticed).
"""

import os
import time
from collections import defaultdict, deque
from datetime import datetime, timedelta, timezone
from typing import Deque, Dict, Optional

import bcrypt
from fastapi import Depends, Header, HTTPException
from jose import JWTError, jwt
from pydantic import BaseModel

from .audit.emit import emit
from .audit.payloads import AuthEventPayload, AuthOutcome
from .request_context import current_ip_hash
from .storage import get_storage

# ── Config (fail fast) ───────────────────────────────────────────────────────

JWT_SECRET = os.getenv("JWT_SECRET", "")
if not JWT_SECRET or JWT_SECRET == "change-me-in-production" or len(JWT_SECRET) < 16:
    raise RuntimeError(
        "JWT_SECRET is unset, too short (<16 chars), or still the placeholder. "
        "Set a strong random value before starting the API (blueprint WP 04)."
    )

JWT_ALGORITHM = "HS256"
JWT_EXPIRE_HOURS = int(os.getenv("JWT_EXPIRE_HOURS", "72"))
MIN_PASSWORD_LEN = int(os.getenv("QM_MIN_PASSWORD_LEN", "8"))
_USERS_KEY = "auth/users"

# crude in-process rate limit on /login (WP 04 server tasks)
_LOGIN_ATTEMPTS: Dict[str, Deque[float]] = defaultdict(deque)
_LOGIN_WINDOW_S = 300
_LOGIN_MAX = 10


def _hash_password(password: str) -> str:
    return bcrypt.hashpw(password.encode(), bcrypt.gensalt()).decode()


def _verify_password(password: str, hashed: str) -> bool:
    return bcrypt.checkpw(password.encode(), hashed.encode())


# ── Models ───────────────────────────────────────────────────────────────────


class UserRecord(BaseModel):
    username: str
    hashed_password: str  # "" for accounts created through Google
    created_at: str = ""
    email: str = ""  # only set for Google accounts


class AuthRequest(BaseModel):
    username: str
    password: str


class AuthResponse(BaseModel):
    token: str
    username: str


class UserInfo(BaseModel):
    username: str
    email: Optional[str] = None


# ── User store ───────────────────────────────────────────────────────────────


def _load_users() -> dict[str, UserRecord]:
    raw = get_storage().read_json(_USERS_KEY) or {}
    return {k: UserRecord(**v) for k, v in raw.items()}


def _save_users(users: dict[str, UserRecord]) -> None:
    get_storage().write_json(_USERS_KEY, {k: v.model_dump() for k, v in users.items()})


# ── Token helpers ────────────────────────────────────────────────────────────


def _create_token(username: str) -> str:
    expire = datetime.now(timezone.utc) + timedelta(hours=JWT_EXPIRE_HOURS)
    return jwt.encode(
        {"sub": username, "exp": expire}, JWT_SECRET, algorithm=JWT_ALGORITHM
    )


def _decode_token(token: str) -> Optional[str]:
    try:
        return jwt.decode(token, JWT_SECRET, algorithms=[JWT_ALGORITHM]).get("sub")
    except JWTError:
        return None


def _rate_limit(username: str) -> None:
    now = time.monotonic()
    q = _LOGIN_ATTEMPTS[username]
    while q and now - q[0] > _LOGIN_WINDOW_S:
        q.popleft()
    if len(q) >= _LOGIN_MAX:
        emit(
            "auth.rate_limited",
            AuthEventPayload(
                outcome=AuthOutcome.RATE_LIMITED, ip_hash=current_ip_hash()
            ),
            username=username,
        )
        raise HTTPException(
            status_code=429, detail="Too many attempts. Try again later."
        )
    q.append(now)


# ── Public helpers / dependencies ────────────────────────────────────────────


def register_user(username: str, password: str) -> AuthResponse:
    username = username.strip().lower()
    if len(username) < 2:
        raise HTTPException(status_code=400, detail="Username too short")
    if ":" in username:
        # ':' namespaces provider accounts ("google:<sub>"): a password user must
        # never be able to claim one.
        raise HTTPException(status_code=400, detail="Username cannot contain ':'")
    if len(password) < MIN_PASSWORD_LEN:
        raise HTTPException(
            status_code=400,
            detail=f"Password must be at least {MIN_PASSWORD_LEN} characters",
        )

    users = _load_users()
    if username in users:
        raise HTTPException(status_code=409, detail="Username already taken")

    users[username] = UserRecord(
        username=username,
        hashed_password=_hash_password(password),
        created_at=datetime.now(timezone.utc).isoformat(),
    )
    _save_users(users)
    emit(
        "auth.register",
        AuthEventPayload(outcome=AuthOutcome.REGISTER, ip_hash=current_ip_hash()),
        username=username,
    )
    return AuthResponse(token=_create_token(username), username=username)


def login_user(username: str, password: str) -> AuthResponse:
    username = username.strip().lower()
    _rate_limit(username)
    user = _load_users().get(username)
    if (
        user is None
        or not user.hashed_password  # Google account: no password to check
        or not _verify_password(password, user.hashed_password)
    ):
        emit(
            "auth.login_failed",
            AuthEventPayload(
                outcome=AuthOutcome.LOGIN_FAILED, ip_hash=current_ip_hash()
            ),
            username=username,
        )
        raise HTTPException(status_code=401, detail="Invalid credentials")
    emit(
        "auth.login_ok",
        AuthEventPayload(outcome=AuthOutcome.LOGIN_OK, ip_hash=current_ip_hash()),
        username=username,
    )
    return AuthResponse(token=_create_token(username), username=username)


def login_google(sub: str, email: str) -> AuthResponse:
    """Find or create the account for a verified Google identity, issue a JWT."""
    username = f"google:{sub}"
    users = _load_users()
    user = users.get(username)
    if user is None:
        user = UserRecord(
            username=username,
            hashed_password="",
            created_at=datetime.now(timezone.utc).isoformat(),
            email=email,
        )
        users[username] = user
        _save_users(users)
        emit(
            "auth.register",
            AuthEventPayload(outcome=AuthOutcome.REGISTER, ip_hash=current_ip_hash()),
            username=username,
        )
    else:
        if user.email != email:
            user.email = email  # keep the display email current
            _save_users(users)
        emit(
            "auth.login_ok",
            AuthEventPayload(outcome=AuthOutcome.LOGIN_OK, ip_hash=current_ip_hash()),
            username=username,
        )
    return AuthResponse(token=_create_token(username), username=username)


def user_info(username: str) -> UserInfo:
    user = _load_users().get(username)
    return UserInfo(username=username, email=(user.email or None) if user else None)


def decode_bearer_token(authorization: Optional[str]) -> Optional[str]:
    """Shared by the `optional_user` dependency and `AuditMiddleware`, which
    decodes once per request to populate `username` in the audit context."""
    if not authorization:
        return None
    parts = authorization.split()
    if len(parts) != 2 or parts[0].lower() != "bearer":
        return None
    return _decode_token(parts[1])


def optional_user(authorization: Optional[str] = Header(default=None)) -> Optional[str]:
    return decode_bearer_token(authorization)


def require_user(user: Optional[str] = Depends(optional_user)) -> str:
    if user is None:
        raise HTTPException(status_code=401, detail="Authentication required")
    return user


def _admin_users() -> set[str]:
    return {u.strip() for u in os.getenv("QM_ADMIN_USERS", "").split(",") if u.strip()}


def require_admin(user: str = Depends(require_user)) -> str:
    """Accounts named in `QM_ADMIN_USERS` (comma-separated). Unset, nobody is
    an administrator: an admin route is closed rather than open by default."""
    if user not in _admin_users():
        raise HTTPException(status_code=403, detail="Administrator only")
    return user
