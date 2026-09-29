"""Response models of the credit page (routers/credit.py). Decimals throughout:
0.0123 is 1.23 % or 123 bp."""

from __future__ import annotations

from datetime import date
from typing import List, Literal, Optional

from pydantic import BaseModel, Field

from .schemas import MethodologySection

# ── Market spreads ───────────────────────────────────────────────────────────


class QuotedSpreadPoint(BaseModel):
    series_id: str
    label: str
    tenor: float = Field(
        description="Where the bucket is placed on the maturity axis, years"
    )
    spread: float


class HazardCurveResponse(BaseModel):
    times: List[float] = Field(description="Right ends of the constant-hazard segments")
    hazards: List[float]
    grid: List[float]
    survival: List[float]
    max_repricing_error_bp: float


class RatingSpread(BaseModel):
    series_id: str
    label: str
    group: Literal["IG", "HY"]
    spread: float
    hazard: float
    pd_horizons: List[float]
    default_probabilities: List[float]


class SeriesValue(BaseModel):
    series_id: str
    label: str
    as_of: Optional[date]
    value: Optional[float]


class CreditSpreadsResponse(BaseModel):
    as_of: date
    recovery: float
    discount_curve_as_of: date
    term_structure: List[QuotedSpreadPoint]
    hazard: Optional[HazardCurveResponse]
    hazard_unavailable: Optional[str]
    ratings_as_of: Optional[date]
    ratings: List[RatingSpread]
    aggregates: List[SeriesValue]
    reference_yields: List[SeriesValue]
    methodology: List[MethodologySection]
    warnings: List[str]


class SpreadHistoryPoint(BaseModel):
    date: date
    value: float


class SpreadHistoryResponse(BaseModel):
    series_id: str
    label: str
    kind: Literal["spread", "yield"]
    points: List[SpreadHistoryPoint]


# ── Companies and their statements ───────────────────────────────────────────


class CompanySummary(BaseModel):
    cik: int
    tickers: List[str]
    name: Optional[str]
    sic: Optional[str]
    sic_description: Optional[str]
    last_filed: date


class CompaniesResponse(BaseModel):
    companies: List[CompanySummary]


class FigureSource(BaseModel):
    concept: str
    filed: date
    accession: str
    url: Optional[str]


class FigureCell(BaseModel):
    value: float
    #: The filing a tagged figure comes from; for a derived figure, the
    #: filings of its inputs.
    sources: List[FigureSource]
    formula: Optional[str] = None


class StatementRow(BaseModel):
    key: str
    label: str
    statement: Literal["income", "balance", "cash_flow", "ratio"]
    unit: str
    derived: bool
    formula: Optional[str]
    values: List[Optional[FigureCell]]


class FilingLink(BaseModel):
    accession: str
    form: str
    filed: date
    report_date: Optional[date]
    url: Optional[str]


class FundamentalsResponse(BaseModel):
    company: CompanySummary
    frequency: Literal["annual", "quarterly"]
    periods: List[date] = Field(description="Period ends, most recent first")
    rows: List[StatementRow]
    ratios: List[StatementRow]
    filings: List[FilingLink]
    methodology: List[MethodologySection]


# ── Structural model ─────────────────────────────────────────────────────────


class StructuralInputsResponse(BaseModel):
    price: float
    price_date: date
    shares_outstanding: float
    shares_as_of: date
    shares_source: FigureSource
    equity_value: float
    equity_vol: float
    vol_observations: int
    balance_sheet_date: date
    short_term_debt: Optional[FigureCell]
    long_term_debt: Optional[FigureCell]
    default_point: float
    rate: float


class StructuralResponse(BaseModel):
    company: CompanySummary
    ticker: str
    inputs: Optional[StructuralInputsResponse]
    unavailable: Optional[str]
    asset_value: Optional[float] = None
    asset_vol: Optional[float] = None
    leverage: Optional[float] = None
    iterations: Optional[int] = None
    maturities: Optional[List[float]] = None
    spreads: Optional[List[float]] = None
    default_probabilities: Optional[List[float]] = None
    distances_to_default: Optional[List[float]] = None
    expected_recoveries: Optional[List[float]] = None
    methodology: List[MethodologySection]
    warnings: List[str]
