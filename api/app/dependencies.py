import os

from fastapi import Header, HTTPException


def require_api_key(x_api_key: str | None = Header(default=None)) -> None:
    expected = os.getenv("API_KEY")
    if expected and x_api_key != expected:
        raise HTTPException(status_code=401, detail="Unauthorized")


def bq_project_id() -> str:
    project_id = os.getenv("BQ_PROJECT_ID") or os.getenv("PROJECT_ID")
    if not project_id:
        raise HTTPException(status_code=500, detail="BQ project is not configured")
    return project_id


def bq_table_ref() -> str:
    project_id = bq_project_id()
    dataset_id = os.getenv("BQ_DATASET_ID", "financial_data")
    table_id = os.getenv("BQ_TABLE_ID", "sp500_data")
    return f"{project_id}.{dataset_id}.{table_id}"


def fred_api_key() -> str:
    key = os.getenv("FRED_API_KEY")
    
    if not key:
        raise HTTPException(status_code=500, detail="FRED API key is not configured")
    return key
