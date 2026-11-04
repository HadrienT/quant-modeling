"""Router — site settings every visitor reads (today: the hidden pages)."""

from __future__ import annotations

from fastapi import APIRouter
from pydantic import BaseModel
from starlette.concurrency import run_in_threadpool

from .. import site_pages

router = APIRouter(prefix="/api/site", tags=["site"])


class PublicSitePages(BaseModel):
    hidden: list[str]


@router.get("/pages", response_model=PublicSitePages)
async def get_site_pages() -> PublicSitePages:
    """The pages hidden from visitors. Who hid them, and when, is for admins."""
    pages = await run_in_threadpool(site_pages.load)
    return PublicSitePages(hidden=pages.hidden)
