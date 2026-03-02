"""GCS-backed portfolio persistence (user-scoped)."""

import json
import os
from typing import List, Optional

from google.cloud import storage

from .portfolio_schemas import Portfolio, PortfolioSummary


_BUCKET_NAME = os.getenv("GCS_PORTFOLIO_BUCKET", "quant-portfolios")
_BASE_PREFIX = os.getenv("GCS_PORTFOLIO_PREFIX", "portfolios/")


def _client() -> storage.Client:
    creds_path = os.getenv("GOOGLE_APPLICATION_CREDENTIALS")
    if creds_path:
        return storage.Client.from_service_account_json(creds_path)
    return storage.Client()


def _bucket() -> storage.Bucket:
    return _client().bucket(_BUCKET_NAME)


def _prefix_for(owner: str) -> str:
    """Return GCS prefix scoped to a user. Each user gets their own folder."""
    safe = owner.strip().lower().replace("/", "_").replace("..", "_")
    return f"{_BASE_PREFIX}{safe}/"


def _blob_name(portfolio_id: str, owner: str) -> str:
    return f"{_prefix_for(owner)}{portfolio_id}.json"


# ---------------------------------------------------------------------------
# CRUD (all scoped to owner)
# ---------------------------------------------------------------------------

def list_portfolios(owner: str) -> List[PortfolioSummary]:
    """List all portfolio summaries for a given user."""
    bucket = _bucket()
    prefix = _prefix_for(owner)
    blobs = bucket.list_blobs(prefix=prefix)
    summaries: List[PortfolioSummary] = []
    for blob in blobs:
        if not blob.name.endswith(".json"):
            continue
        try:
            data = json.loads(blob.download_as_text())
            pf = Portfolio(**data)
            total_val = sum(
                (p.result.npv if p.result else 0.0)
                for p in pf.positions
            )
            summaries.append(PortfolioSummary(
                id=pf.id,
                name=pf.name,
                created_at=pf.created_at,
                updated_at=pf.updated_at,
                n_positions=len(pf.positions),
                total_value=total_val,
            ))
        except Exception:
            continue
    return summaries


def get_portfolio(portfolio_id: str, owner: str) -> Optional[Portfolio]:
    """Load a portfolio from GCS (must belong to owner)."""
    bucket = _bucket()
    blob = bucket.blob(_blob_name(portfolio_id, owner))
    if not blob.exists():
        return None
    data = json.loads(blob.download_as_text())
    pf = Portfolio(**data)
    # Double-check ownership
    if pf.owner and pf.owner != owner:
        return None
    return pf


def save_portfolio(portfolio: Portfolio) -> None:
    """Save / overwrite a portfolio in GCS (stored under owner prefix)."""
    bucket = _bucket()
    blob = bucket.blob(_blob_name(portfolio.id, portfolio.owner))
    blob.upload_from_string(
        portfolio.model_dump_json(indent=2),
        content_type="application/json",
    )


def delete_portfolio(portfolio_id: str, owner: str) -> bool:
    """Delete a portfolio from GCS. Returns True if deleted."""
    bucket = _bucket()
    blob = bucket.blob(_blob_name(portfolio_id, owner))
    if not blob.exists():
        return False
    blob.delete()
    return True
