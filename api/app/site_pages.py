"""Which pages of the site are shown to visitors.

An administrator hides a page from the admin panel; the front drops it from
the navigation and the command palette, and a visitor who follows a link to it
sees a "not available" page. Administrators keep seeing every page.

This is presentation, not access control: the API routes behind a hidden page
stay open. The list lives in the key/value store (`storage.py`), so it
survives a restart and is shared by every worker.
"""

from __future__ import annotations

from datetime import datetime, timezone

from pydantic import BaseModel, Field

from .storage import get_storage

#: The pages that can be hidden: the navigation's (`web/src/shared/config/pages.ts`,
#: checked by `api/tests/test_site_pages.py`). The home page, the legal pages,
#: the profile and the admin panel itself are always shown.
HIDEABLE_PAGES: tuple[str, ...] = (
    "/price",
    "/scripting",
    "/market",
    "/credit",
    "/xva",
    "/simulation",
    "/portfolio",
    "/backtest",
    "/visualize",
    "/products",
    "/about",
)

_KEY = "site/pages"


class SitePages(BaseModel):
    hidden: list[str] = Field(default_factory=list)
    updated_at: str | None = None
    updated_by: str | None = None


class SitePagesUpdate(BaseModel):
    hidden: list[str]


class UnknownPage(ValueError):
    pass


def load() -> SitePages:
    raw = get_storage().read_json(_KEY)
    if raw is None:
        return SitePages()
    pages = SitePages.model_validate(raw)
    # A page removed from the site since it was hidden must not linger.
    pages.hidden = [p for p in pages.hidden if p in HIDEABLE_PAGES]
    return pages


def save(hidden: list[str], *, by: str) -> SitePages:
    unknown = sorted(set(hidden) - set(HIDEABLE_PAGES))
    if unknown:
        raise UnknownPage(f"cannot hide unknown page(s): {', '.join(unknown)}")
    pages = SitePages(
        # Navigation order, without duplicates.
        hidden=[p for p in HIDEABLE_PAGES if p in set(hidden)],
        updated_at=datetime.now(timezone.utc).isoformat(timespec="seconds"),
        updated_by=by,
    )
    get_storage().write_json(_KEY, pages.model_dump())
    return pages
