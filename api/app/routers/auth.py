"""Auth endpoints: register, login, me, and Sign in with Google."""

from typing import Optional
from urllib.parse import quote

from fastapi import APIRouter, Cookie, Depends, HTTPException, Query
from fastapi.responses import RedirectResponse
from pydantic import BaseModel

from .. import google_oauth
from ..auth import (
    AuthRequest,
    AuthResponse,
    UserInfo,
    login_google,
    login_user,
    register_user,
    require_user,
    user_info,
)

router = APIRouter(prefix="/api/auth", tags=["auth"])


class Providers(BaseModel):
    google: bool


@router.post("/register", response_model=AuthResponse, status_code=201)
async def api_register(body: AuthRequest):
    return register_user(body.username, body.password)


@router.post("/login", response_model=AuthResponse)
async def api_login(body: AuthRequest):
    return login_user(body.username, body.password)


@router.get("/me", response_model=UserInfo)
async def api_me(user: str = Depends(require_user)):
    # 401 on an invalid/absent token (was 200 with an empty username — WP 04).
    return user_info(user)


@router.get("/providers", response_model=Providers)
async def api_providers():
    """Which sign-in methods are configured, so the UI only offers real ones."""
    return Providers(google=google_oauth.is_enabled())


@router.get("/google/login", include_in_schema=False)
def api_google_login():
    url, state_cookie = google_oauth.start_login()
    resp = RedirectResponse(url, status_code=302)
    resp.set_cookie(
        google_oauth.STATE_COOKIE,
        state_cookie,
        max_age=google_oauth.STATE_TTL_S,
        httponly=True,
        secure=google_oauth.cookie_secure(),
        samesite="lax",  # must accompany the top-level redirect back from Google
        path="/api/auth/google",
    )
    return resp


def _to_app(fragment: str) -> RedirectResponse:
    """Hand the result to the SPA in the URL fragment (never sent to a server)."""
    resp = RedirectResponse(f"/#{fragment}", status_code=302)
    resp.delete_cookie(google_oauth.STATE_COOKIE, path="/api/auth/google")
    return resp


@router.get("/google/callback", include_in_schema=False)
def api_google_callback(
    code: Optional[str] = Query(default=None),
    state: Optional[str] = Query(default=None),
    error: Optional[str] = Query(default=None),
    qm_oauth_state: Optional[str] = Cookie(default=None),
):
    if error or not code or not state:
        # e.g. the user pressed "Cancel" on Google's consent screen
        return _to_app("qm_auth_error=" + quote(error or "missing_code"))
    try:
        profile = google_oauth.complete_login(
            code=code, state=state, state_cookie=qm_oauth_state
        )
    except HTTPException as exc:
        return _to_app("qm_auth_error=" + quote(str(exc.detail)))
    session = login_google(profile.sub, profile.email)
    return _to_app("qm_auth=" + session.token)
