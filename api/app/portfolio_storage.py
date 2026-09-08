"""User-scoped portfolio persistence on the pluggable Storage backend (WP 04)."""

from typing import List, Optional

from .portfolio_schemas import Portfolio, PortfolioSummary
from .storage import get_storage

_BASE_PREFIX = "portfolios"


def _safe(owner: str) -> str:
    return owner.strip().lower().replace("/", "_").replace("..", "_") or "_anon"


def _key(portfolio_id: str, owner: str) -> str:
    return f"{_BASE_PREFIX}/{_safe(owner)}/{portfolio_id}"


def _prefix_for(owner: str) -> str:
    return f"{_BASE_PREFIX}/{_safe(owner)}/"


def list_portfolios(owner: str) -> List[PortfolioSummary]:
    store = get_storage()
    summaries: List[PortfolioSummary] = []
    for key in store.list_keys(_prefix_for(owner)):
        data = store.read_json(key)
        if not data:
            continue
        try:
            pf = Portfolio(**data)
        except Exception:
            continue
        total_val = sum((p.result.npv if p.result else 0.0) for p in pf.positions)
        summaries.append(
            PortfolioSummary(
                id=pf.id,
                name=pf.name,
                created_at=pf.created_at,
                updated_at=pf.updated_at,
                n_positions=len(pf.positions),
                total_value=total_val,
            )
        )
    return summaries


def get_portfolio(portfolio_id: str, owner: str) -> Optional[Portfolio]:
    data = get_storage().read_json(_key(portfolio_id, owner))
    if not data:
        return None
    pf = Portfolio(**data)
    if pf.owner and pf.owner != _safe(owner):
        return None
    return pf


def save_portfolio(portfolio: Portfolio) -> None:
    get_storage().write_json(
        _key(portfolio.id, portfolio.owner), portfolio.model_dump()
    )


def delete_portfolio(portfolio_id: str, owner: str) -> bool:
    return get_storage().delete(_key(portfolio_id, owner))
