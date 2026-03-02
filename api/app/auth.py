"""Lightweight JWT auth with users stored in GCS."""

import json
import os
from datetime import datetime, timedelta, timezone
from typing import Optional

import bcrypt
from fastapi import Depends, Header, HTTPException
from jose import JWTError, jwt
from pydantic import BaseModel

from google.cloud import storage


# ── Config ───────────────────────────────────────────────────────────────────

JWT_SECRET = os.getenv("JWT_SECRET", "change-me-in-production")
JWT_ALGORITHM = "HS256"
JWT_EXPIRE_HOURS = int(os.getenv("JWT_EXPIRE_HOURS", "72"))

_BUCKET_NAME = os.getenv("GCS_PORTFOLIO_BUCKET", "quant-portfolios")
_USERS_BLOB = "auth/users.json"

def _hash_password(password: str) -> str:
    return bcrypt.hashpw(password.encode(), bcrypt.gensalt()).decode()

def _verify_password(password: str, hashed: str) -> bool:
    return bcrypt.checkpw(password.encode(), hashed.encode())


# ── Models ───────────────────────────────────────────────────────────────────

class UserRecord(BaseModel):
    username: str
    hashed_password: str
    created_at: str = ""


class AuthRequest(BaseModel):
    username: str
    password: str


class AuthResponse(BaseModel):
    token: str
    username: str


class UserInfo(BaseModel):
    username: str


# ── GCS user store ───────────────────────────────────────────────────────────

def _gcs_client() -> storage.Client:
    creds_path = os.getenv("GOOGLE_APPLICATION_CREDENTIALS")
    if creds_path:
        return storage.Client.from_service_account_json(creds_path)
    return storage.Client()


def _load_users() -> dict[str, UserRecord]:
    """Load all users from GCS. Returns {username: UserRecord}."""
    try:
        bucket = _gcs_client().bucket(_BUCKET_NAME)
        blob = bucket.blob(_USERS_BLOB)
        if not blob.exists():
            return {}
        raw = json.loads(blob.download_as_text())
        return {k: UserRecord(**v) for k, v in raw.items()}
    except Exception:
        return {}


def _save_users(users: dict[str, UserRecord]) -> None:
    """Persist users dict to GCS."""
    bucket = _gcs_client().bucket(_BUCKET_NAME)
    blob = bucket.blob(_USERS_BLOB)
    blob.upload_from_string(
        json.dumps({k: v.model_dump() for k, v in users.items()}, indent=2),
        content_type="application/json",
    )


# ── Token helpers ────────────────────────────────────────────────────────────

def _create_token(username: str) -> str:
    expire = datetime.now(timezone.utc) + timedelta(hours=JWT_EXPIRE_HOURS)
    return jwt.encode(
        {"sub": username, "exp": expire},
        JWT_SECRET,
        algorithm=JWT_ALGORITHM,
    )


def _decode_token(token: str) -> Optional[str]:
    """Return username or None."""
    try:
        payload = jwt.decode(token, JWT_SECRET, algorithms=[JWT_ALGORITHM])
        return payload.get("sub")
    except JWTError:
        return None


# ── Public helpers / dependencies ────────────────────────────────────────────

def register_user(username: str, password: str) -> AuthResponse:
    """Create a new user account."""
    username = username.strip().lower()
    if len(username) < 2:
        raise HTTPException(status_code=400, detail="Username too short")
    if len(password) < 4:
        raise HTTPException(status_code=400, detail="Password too short")

    users = _load_users()
    if username in users:
        raise HTTPException(status_code=409, detail="Username already taken")

    users[username] = UserRecord(
        username=username,
        hashed_password=_hash_password(password),
        created_at=datetime.now(timezone.utc).isoformat(),
    )
    _save_users(users)
    return AuthResponse(token=_create_token(username), username=username)


def login_user(username: str, password: str) -> AuthResponse:
    """Authenticate and return a JWT."""
    username = username.strip().lower()
    users = _load_users()
    user = users.get(username)
    if user is None or not _verify_password(password, user.hashed_password):
        raise HTTPException(status_code=401, detail="Invalid credentials")
    return AuthResponse(token=_create_token(username), username=username)


def optional_user(authorization: Optional[str] = Header(default=None)) -> Optional[str]:
    """Dependency: returns username if a valid Bearer token is present, else None."""
    if not authorization:
        return None
    parts = authorization.split()
    if len(parts) != 2 or parts[0].lower() != "bearer":
        return None
    return _decode_token(parts[1])


def require_user(user: Optional[str] = Depends(optional_user)) -> str:
    """Dependency: requires a valid Bearer token."""
    if user is None:
        raise HTTPException(status_code=401, detail="Authentication required")
    return user
