"""What the GDPR gives an account holder: a copy of their data (art. 15 and
20) and its erasure (art. 17).

An account owns two things on the server: its record in the user store and its
portfolios. The scripting assistant keeps no history server-side (the browser
sends it with each message). The audit trail keeps the username until its
monthly partitions expire (12 months, quant-platform `AUDIT_RETENTION_MONTHS`):
it is append-only by design, and the privacy policy says so.
"""

from __future__ import annotations

from datetime import datetime, timezone
from typing import List

from pydantic import BaseModel

from .auth import UserInfo, forget_user, user_info
from .portfolio_schemas import Portfolio
from .portfolio_storage import delete_all_portfolios, get_portfolio, list_portfolios


class AccountExport(BaseModel):
    exported_at: str
    account: UserInfo
    portfolios: List[Portfolio]
    not_included: List[str]


_NOT_INCLUDED = [
    "Portfolios, presets and preferences saved without an account live in "
    "your browser only; the site never receives them.",
    "Security and valuation logs (audit trail) are kept 12 months, then "
    "deleted; they are not part of this export.",
]


def export_account(username: str) -> AccountExport:
    portfolios = [
        pf
        for summary in list_portfolios(username)
        if (pf := get_portfolio(summary.id, username)) is not None
    ]
    return AccountExport(
        exported_at=datetime.now(timezone.utc).isoformat(),
        account=user_info(username),
        portfolios=portfolios,
        not_included=_NOT_INCLUDED,
    )


def delete_account(username: str) -> None:
    """Data first, then the record: an interruption in between leaves an
    account without portfolios, never portfolios without an account."""
    delete_all_portfolios(username)
    forget_user(username)
