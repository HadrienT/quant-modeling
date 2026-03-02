"""Auth endpoints: register, login, me."""

from fastapi import APIRouter, Depends

from ..auth import (
    AuthRequest,
    AuthResponse,
    UserInfo,
    login_user,
    optional_user,
    register_user,
)

router = APIRouter(prefix="/api/auth", tags=["auth"])


@router.post("/register", response_model=AuthResponse, status_code=201)
async def api_register(body: AuthRequest):
    return register_user(body.username, body.password)


@router.post("/login", response_model=AuthResponse)
async def api_login(body: AuthRequest):
    return login_user(body.username, body.password)


@router.get("/me", response_model=UserInfo)
async def api_me(user: str = Depends(optional_user)):
    if user is None:
        return UserInfo(username="")
    return UserInfo(username=user)
