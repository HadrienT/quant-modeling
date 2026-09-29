"""Router — the credit page: market credit spreads, company fundamentals
(10-K / 10-Q) and the structural credit model.

GET /api/credit/spreads/overview?recovery=0.4        spread curves, hazard curve, ratings
GET /api/credit/spreads/history?series_id=…&years=3  one spread or yield series
GET /api/credit/companies                            companies with filings in the store
GET /api/credit/companies/{ticker}/fundamentals      statements, ratios, filings
GET /api/credit/companies/{ticker}/structural        Merton model for the company

The computation and the methodology text live in `credit.py` and
`fundamentals.py`; this module only caches and shapes the responses.
"""

from dataclasses import asdict
from typing import List, Literal, Optional

from fastapi import APIRouter, HTTPException, Path, Query

from .. import credit, db, fundamentals
from ..cache import TTLCache
from ..credit_schemas import (
    CompaniesResponse,
    CompanySummary,
    CreditSpreadsResponse,
    FigureCell,
    FigureSource,
    FilingLink,
    FundamentalsResponse,
    HazardCurveResponse,
    QuotedSpreadPoint,
    RatingSpread,
    SeriesValue,
    SpreadHistoryPoint,
    SpreadHistoryResponse,
    StatementRow,
    StructuralInputsResponse,
    StructuralResponse,
)
from ..request_context import set_cache_hit
from ..schemas import MethodologySection

router = APIRouter()

# Spreads publish daily and filings weekly: an hour of cache costs nothing.
_SPREADS_CACHE = TTLCache[str, CreditSpreadsResponse](max_size=20, ttl_seconds=3600)
_HISTORY_CACHE = TTLCache[str, SpreadHistoryResponse](max_size=60, ttl_seconds=3600)
_COMPANIES_CACHE = TTLCache[str, CompaniesResponse](max_size=1, ttl_seconds=3600)
_FUNDAMENTALS_CACHE = TTLCache[str, FundamentalsResponse](
    max_size=200, ttl_seconds=3600
)
_STRUCTURAL_CACHE = TTLCache[str, StructuralResponse](max_size=200, ttl_seconds=3600)

TickerParam = Path(..., pattern=r"^[A-Za-z0-9.\-]{1,10}$")


def _methodology(sections) -> List[MethodologySection]:
    return [MethodologySection(title=t, paragraphs=p) for t, p in sections]


def _company(c: db.SecCompany) -> CompanySummary:
    return CompanySummary(
        cik=c.cik,
        tickers=list(c.tickers),
        name=c.name,
        sic=c.sic,
        sic_description=c.sic_description,
        last_filed=c.last_filed,
    )


def _store_error(exc: Exception) -> HTTPException:
    return HTTPException(status_code=503, detail="Market data store unavailable")


@router.get("/api/credit/spreads/overview", response_model=CreditSpreadsResponse)
def credit_spreads_overview(
    recovery: float = Query(
        0.4, ge=0.0, le=0.9, description="Recovery rate assumption."
    ),
) -> CreditSpreadsResponse:
    key = f"{recovery:.4f}"
    cached = _SPREADS_CACHE.get(key)
    if cached is not None:
        set_cache_hit()
        return cached
    try:
        o = credit.spreads_overview(recovery)
    except db.StoreUnavailable as exc:
        raise _store_error(exc) from exc
    except credit.CreditUnavailable as exc:
        raise HTTPException(status_code=503, detail=str(exc)) from exc

    def values(rows):
        return [
            SeriesValue(series_id=s.series_id, label=s.label, as_of=d, value=v)
            for s, d, v in rows
        ]

    response = CreditSpreadsResponse(
        as_of=o.as_of,
        recovery=o.recovery,
        discount_curve_as_of=o.discount_as_of,
        term_structure=[QuotedSpreadPoint(**vars(q)) for q in o.term_structure],
        hazard=None if o.hazard is None else HazardCurveResponse(**vars(o.hazard)),
        hazard_unavailable=o.hazard_unavailable,
        ratings_as_of=o.ratings_as_of,
        ratings=[
            RatingSpread(
                series_id=r.series_id,
                label=r.label,
                group=r.group,
                spread=r.spread,
                hazard=r.hazard,
                pd_horizons=list(credit.PD_HORIZONS),
                default_probabilities=r.default_probabilities,
            )
            for r in o.ratings
        ],
        aggregates=values(o.aggregates),
        reference_yields=values(o.reference_yields),
        methodology=_methodology(credit.SPREADS_METHODOLOGY),
        warnings=o.warnings,
    )
    _SPREADS_CACHE.set(key, response)
    return response


@router.get("/api/credit/spreads/history", response_model=SpreadHistoryResponse)
def credit_spread_history(
    series_id: str = Query(..., description="A spread or reference-yield series id."),
    years: int = Query(3, ge=1, le=40),
) -> SpreadHistoryResponse:
    key = f"{series_id}:{years}"
    cached = _HISTORY_CACHE.get(key)
    if cached is not None:
        set_cache_hit()
        return cached
    try:
        points = credit.spread_history(series_id, years)
    except KeyError as exc:
        raise HTTPException(
            status_code=404, detail=f"Unknown series {series_id}"
        ) from exc
    except db.StoreUnavailable as exc:
        raise _store_error(exc) from exc
    spec = credit.HISTORY_SERIES[series_id]
    response = SpreadHistoryResponse(
        series_id=series_id,
        label=spec.label,
        kind="yield" if spec.group == "yield" else "spread",
        points=[SpreadHistoryPoint(date=d, value=v) for d, v in points],
    )
    _HISTORY_CACHE.set(key, response)
    return response


@router.get("/api/credit/companies", response_model=CompaniesResponse)
def credit_companies() -> CompaniesResponse:
    cached = _COMPANIES_CACHE.get("all")
    if cached is not None:
        set_cache_hit()
        return cached
    try:
        companies = db.sec_companies()
    except db.StoreUnavailable as exc:
        raise _store_error(exc) from exc
    response = CompaniesResponse(
        companies=sorted((_company(c) for c in companies), key=lambda c: c.tickers[0])
    )
    _COMPANIES_CACHE.set("all", response)
    return response


def _sources(fig: fundamentals.Figure, urls: dict) -> List[FigureSource]:
    if fig.concept is not None:
        return [
            FigureSource(
                concept=fig.concept,
                filed=fig.filed,
                accession=fig.accession,
                url=urls.get(fig.accession),
            )
        ]
    out: List[FigureSource] = []
    for g in fig.inputs:
        out.extend(_sources(g, urls))
    return out


def _cell(fig: Optional[fundamentals.Figure], urls: dict) -> Optional[FigureCell]:
    if fig is None:
        return None
    return FigureCell(value=fig.value, sources=_sources(fig, urls), formula=fig.formula)


def _row(r: fundamentals.Row, urls: dict) -> StatementRow:
    return StatementRow(
        key=r.key,
        label=r.label,
        statement=r.statement,
        unit=r.unit,
        derived=r.derived,
        formula=r.formula,
        values=[_cell(v, urls) for v in r.values],
    )


@router.get(
    "/api/credit/companies/{ticker}/fundamentals", response_model=FundamentalsResponse
)
def credit_company_fundamentals(
    ticker: str = TickerParam,
    frequency: Literal["annual", "quarterly"] = Query("annual"),
    periods: int = Query(6, ge=1, le=20),
) -> FundamentalsResponse:
    key = f"{ticker.upper()}:{frequency}:{periods}"
    cached = _FUNDAMENTALS_CACHE.get(key)
    if cached is not None:
        set_cache_hit()
        return cached
    try:
        s = fundamentals.statements(ticker, frequency, periods)
    except fundamentals.CompanyNotFound as exc:
        raise HTTPException(
            status_code=404, detail=f"No filings stored for {ticker}"
        ) from exc
    except db.StoreUnavailable as exc:
        raise _store_error(exc) from exc
    response = FundamentalsResponse(
        company=_company(s.company),
        frequency=frequency,
        periods=s.periods,
        rows=[_row(r, s.urls) for r in s.rows],
        ratios=[_row(r, s.urls) for r in s.ratios],
        filings=[FilingLink(**asdict(f)) for f in s.filings],
        methodology=_methodology(credit.FUNDAMENTALS_METHODOLOGY),
    )
    _FUNDAMENTALS_CACHE.set(key, response)
    return response


@router.get(
    "/api/credit/companies/{ticker}/structural", response_model=StructuralResponse
)
def credit_company_structural(ticker: str = TickerParam) -> StructuralResponse:
    key = ticker.upper()
    cached = _STRUCTURAL_CACHE.get(key)
    if cached is not None:
        set_cache_hit()
        return cached
    try:
        r = credit.structural(ticker)
    except fundamentals.CompanyNotFound as exc:
        raise HTTPException(
            status_code=404, detail=f"No filings stored for {ticker}"
        ) from exc
    except db.StoreUnavailable as exc:
        raise _store_error(exc) from exc
    except credit.CreditUnavailable as exc:
        raise HTTPException(status_code=503, detail=str(exc)) from exc

    inputs = None
    if r.inputs is not None:
        i = r.inputs
        urls = db.sec_filing_urls(
            r.company.cik,
            [i.shares.accession]
            + [
                s.accession
                for f in (
                    i.balance_sheet.short_term_debt,
                    i.balance_sheet.long_term_debt,
                )
                if f is not None
                for s in ([f] if f.concept else f.inputs)
            ],
        )
        inputs = StructuralInputsResponse(
            price=i.price,
            price_date=i.price_date,
            shares_outstanding=i.shares.shares,
            shares_as_of=i.shares.as_of,
            shares_source=FigureSource(
                concept=i.shares.concept,
                filed=i.shares.filed,
                accession=i.shares.accession,
                url=urls.get(i.shares.accession),
            ),
            equity_value=i.equity_value,
            equity_vol=i.equity_vol,
            vol_observations=i.vol_observations,
            balance_sheet_date=i.balance_sheet.period_end,
            short_term_debt=_cell(i.balance_sheet.short_term_debt, urls),
            long_term_debt=_cell(i.balance_sheet.long_term_debt, urls),
            default_point=i.default_point,
            rate=i.rate,
        )
    response = StructuralResponse(
        company=_company(r.company),
        ticker=r.ticker,
        inputs=inputs,
        unavailable=r.unavailable,
        asset_value=r.asset_value,
        asset_vol=r.asset_vol,
        leverage=r.leverage,
        iterations=r.iterations,
        maturities=r.maturities,
        spreads=r.spreads,
        default_probabilities=r.default_probabilities,
        distances_to_default=r.distances_to_default,
        expected_recoveries=r.expected_recoveries,
        methodology=_methodology(credit.STRUCTURAL_METHODOLOGY),
        warnings=r.warnings or [],
    )
    _STRUCTURAL_CACHE.set(key, response)
    return response
