"""Financial statements of US companies, from their 10-K and 10-Q filings.

Read from `fundamentals.sec_facts` (data-ingest's sec-fundamentals source:
SEC EDGAR XBRL, S&P 500). data-ingest stores the XBRL concepts raw; deciding
which concept is a company's "revenue" happens here, in one table
(`LINE_ITEMS`), where the page can show it.

Three rules, each visible on the page:

* **One line, several candidate concepts, a fixed priority.** Companies tag
  the same line differently (revenue is `Revenues` for some,
  `RevenueFromContractWithCustomerExcludingAssessedTax` for others). The first
  candidate with a value for the period wins, and the concept used is returned
  with the figure.
* **As currently reported.** A 10-K restates prior years; when a period has
  figures from several filings, the most recent filing's is shown, with that
  filing's date and link. Older vintages stay in the store (point-in-time).
* **Nothing is estimated.** A line a company does not tag is empty. Derived
  lines (total debt, free cash flow, EBITDA, ratios) carry their formula, and
  are empty when an input is.

Periods: *annual* = fiscal years, a flow over 350–380 days and a balance
sheet at the fiscal year end; *quarterly* = a flow over 80–100 days and the
balance sheet at the quarter end. A fourth fiscal quarter is not filed on a
10-Q: companies that do not tag it in their 10-K have no Q4 flows, and it is
not derived as the year minus nine months.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from datetime import date
from typing import Callable, Dict, List, Literal, Optional, Sequence, Tuple

from . import db

Frequency = Literal["annual", "quarterly"]
ANNUAL_FORMS = ("10-K", "10-K/A")

#: Flow durations, in days, that count as a year / a quarter (52/53-week
#: fiscal years and 13/14-week quarters included).
DURATIONS = {"annual": (350, 380), "quarterly": (80, 100)}


@dataclass(frozen=True)
class LineItem:
    key: str
    label: str
    statement: Literal["income", "balance", "cash_flow"]
    kind: Literal["flow", "instant"]
    candidates: Tuple[str, ...]
    unit: str = "USD"  # "USD", "USD/shares", "shares"


def _li(key, label, statement, kind, *candidates, unit="USD") -> LineItem:
    return LineItem(key, label, statement, kind, tuple(candidates), unit)


LINE_ITEMS: Tuple[LineItem, ...] = (
    # Income statement
    _li(
        "revenue",
        "Revenue",
        "income",
        "flow",
        "Revenues",
        "RevenueFromContractWithCustomerExcludingAssessedTax",
        "RevenueFromContractWithCustomerIncludingAssessedTax",
        "SalesRevenueNet",
    ),
    _li(
        "cost_of_revenue",
        "Cost of revenue",
        "income",
        "flow",
        "CostOfRevenue",
        "CostOfGoodsAndServicesSold",
    ),
    _li("gross_profit", "Gross profit", "income", "flow", "GrossProfit"),
    _li(
        "rnd",
        "Research and development",
        "income",
        "flow",
        "ResearchAndDevelopmentExpense",
    ),
    _li(
        "sga",
        "Selling, general and administrative",
        "income",
        "flow",
        "SellingGeneralAndAdministrativeExpense",
    ),
    _li(
        "operating_income", "Operating income", "income", "flow", "OperatingIncomeLoss"
    ),
    _li(
        "interest_expense",
        "Interest expense",
        "income",
        "flow",
        "InterestExpense",
        "InterestExpenseNonoperating",
        "InterestExpenseDebt",
    ),
    _li(
        "pretax_income",
        "Income before taxes",
        "income",
        "flow",
        "IncomeLossFromContinuingOperationsBeforeIncomeTaxesExtraordinaryItemsNoncontrollingInterest",
    ),
    _li("income_tax", "Income tax", "income", "flow", "IncomeTaxExpenseBenefit"),
    _li("net_income", "Net income", "income", "flow", "NetIncomeLoss"),
    _li(
        "eps_diluted",
        "Diluted EPS",
        "income",
        "flow",
        "EarningsPerShareDiluted",
        unit="USD/shares",
    ),
    _li(
        "diluted_shares",
        "Diluted shares (weighted)",
        "income",
        "flow",
        "WeightedAverageNumberOfDilutedSharesOutstanding",
        unit="shares",
    ),
    _li(
        "d_and_a",
        "Depreciation and amortization",
        "income",
        "flow",
        "DepreciationDepletionAndAmortization",
        "DepreciationAndAmortization",
        "DepreciationAmortizationAndAccretionNet",
    ),
    # Balance sheet
    _li(
        "cash",
        "Cash and equivalents",
        "balance",
        "instant",
        "CashAndCashEquivalentsAtCarryingValue",
        "CashCashEquivalentsRestrictedCashAndRestrictedCashEquivalents",
    ),
    _li(
        "short_term_investments",
        "Short-term investments",
        "balance",
        "instant",
        "ShortTermInvestments",
        "MarketableSecuritiesCurrent",
    ),
    _li(
        "receivables",
        "Receivables",
        "balance",
        "instant",
        "AccountsReceivableNetCurrent",
    ),
    _li("inventory", "Inventory", "balance", "instant", "InventoryNet"),
    _li("current_assets", "Current assets", "balance", "instant", "AssetsCurrent"),
    _li(
        "ppe",
        "Property, plant and equipment",
        "balance",
        "instant",
        "PropertyPlantAndEquipmentNet",
    ),
    _li("goodwill", "Goodwill", "balance", "instant", "Goodwill"),
    _li("total_assets", "Total assets", "balance", "instant", "Assets"),
    _li(
        "current_liabilities",
        "Current liabilities",
        "balance",
        "instant",
        "LiabilitiesCurrent",
    ),
    _li("total_liabilities", "Total liabilities", "balance", "instant", "Liabilities"),
    _li(
        "operating_leases",
        "Operating lease liabilities",
        "balance",
        "instant",
        "OperatingLeaseLiability",
    ),
    _li(
        "equity",
        "Shareholders' equity",
        "balance",
        "instant",
        "StockholdersEquity",
        "StockholdersEquityIncludingPortionAttributableToNoncontrollingInterest",
    ),
    _li(
        "retained_earnings",
        "Retained earnings",
        "balance",
        "instant",
        "RetainedEarningsAccumulatedDeficit",
    ),
    # Cash flow statement
    _li(
        "cfo",
        "Operating cash flow",
        "cash_flow",
        "flow",
        "NetCashProvidedByUsedInOperatingActivities",
    ),
    _li(
        "cfi",
        "Investing cash flow",
        "cash_flow",
        "flow",
        "NetCashProvidedByUsedInInvestingActivities",
    ),
    _li(
        "cff",
        "Financing cash flow",
        "cash_flow",
        "flow",
        "NetCashProvidedByUsedInFinancingActivities",
    ),
    _li(
        "capex",
        "Capital expenditure",
        "cash_flow",
        "flow",
        "PaymentsToAcquirePropertyPlantAndEquipment",
    ),
    _li(
        "dividends",
        "Dividends paid",
        "cash_flow",
        "flow",
        "PaymentsOfDividends",
        "PaymentsOfDividendsCommonStock",
    ),
    _li(
        "buybacks",
        "Share repurchases",
        "cash_flow",
        "flow",
        "PaymentsForRepurchaseOfCommonStock",
    ),
    _li(
        "sbc", "Share-based compensation", "cash_flow", "flow", "ShareBasedCompensation"
    ),
)

#: Debt concepts, read by the two debt lines' own rules below.
DEBT_CONCEPTS = (
    "DebtCurrent",
    "LongTermDebtCurrent",
    "LongTermDebtAndCapitalLeaseObligationsCurrent",
    "ShortTermBorrowings",
    "CommercialPaper",
    "LongTermDebtNoncurrent",
    "LongTermDebtAndCapitalLeaseObligations",
    "LongTermDebt",
)

SHARES_CONCEPTS = ("EntityCommonStockSharesOutstanding", "CommonStockSharesOutstanding")

ALL_CONCEPTS = tuple(
    sorted(
        {c for item in LINE_ITEMS for c in item.candidates}
        | set(DEBT_CONCEPTS)
        | set(SHARES_CONCEPTS)
    )
)


class CompanyNotFound(LookupError):
    pass


# ── Figures and their provenance ─────────────────────────────────────────────


@dataclass
class Figure:
    value: float
    concept: Optional[str] = None  # None for a derived figure
    filed: Optional[date] = None
    accession: Optional[str] = None
    #: For a derived figure: how it was obtained, in words.
    formula: Optional[str] = None
    inputs: List["Figure"] = field(default_factory=list)


class FactIndex:
    """A company's facts, indexed for the two lookups statements need: a flow
    of a given length ending on a date, and a balance at a date."""

    def __init__(self, facts: Sequence[db.SecFact]):
        self.facts = facts
        self._by_concept: Dict[str, List[db.SecFact]] = {}
        for f in facts:
            self._by_concept.setdefault(f.concept, []).append(f)

    def _latest(self, candidates: Sequence[db.SecFact]) -> Optional[db.SecFact]:
        # Most recent filing wins ("as currently reported"); accession breaks ties.
        return max(candidates, key=lambda f: (f.filed, f.accession), default=None)

    def value(
        self, concept: str, end: date, kind: str, frequency: Frequency
    ) -> Optional[Figure]:
        lo, hi = DURATIONS[frequency]
        rows = [
            f
            for f in self._by_concept.get(concept, ())
            if f.period_end == end
            and (
                f.duration_days == 0
                if kind == "instant"
                else lo <= f.duration_days <= hi
            )
        ]
        latest = self._latest(rows)
        if latest is None:
            return None
        return Figure(latest.value, concept, latest.filed, latest.accession)

    def first(
        self, candidates: Sequence[str], end: date, kind: str, frequency: Frequency
    ) -> Optional[Figure]:
        for concept in candidates:
            fig = self.value(concept, end, kind, frequency)
            if fig is not None:
                return fig
        return None

    def period_ends(self, frequency: Frequency) -> List[date]:
        """Fiscal year ends (balance sheets filed on a 10-K) or quarter ends
        (every balance sheet filed), most recent first."""
        assets = self._by_concept.get("Assets", ())
        if frequency == "annual":
            ends = {f.period_end for f in assets if f.form in ANNUAL_FORMS}
        else:
            ends = {f.period_end for f in assets}
        return sorted(ends, reverse=True)


def _derived(value: float, formula: str, *inputs: Figure) -> Figure:
    return Figure(value, formula=formula, inputs=list(inputs))


def short_term_debt(
    idx: FactIndex, end: date, frequency: Frequency
) -> Optional[Figure]:
    """DebtCurrent when tagged; otherwise the current portion of long-term debt
    plus short-term borrowings (or, without those, commercial paper — which
    many filers include in short-term borrowings, so never both)."""
    direct = idx.value("DebtCurrent", end, "instant", frequency)
    if direct is not None:
        return direct
    parts = [
        idx.first(
            ("LongTermDebtCurrent", "LongTermDebtAndCapitalLeaseObligationsCurrent"),
            end,
            "instant",
            frequency,
        ),
        idx.first(
            ("ShortTermBorrowings", "CommercialPaper"), end, "instant", frequency
        ),
    ]
    present = [p for p in parts if p is not None]
    if not present:
        return None
    return _derived(
        sum(p.value for p in present),
        "Current portion of long-term debt + short-term borrowings (or commercial paper)",
        *present,
    )


def long_term_debt(idx: FactIndex, end: date, frequency: Frequency) -> Optional[Figure]:
    """The non-current part: LongTermDebtNoncurrent, else
    LongTermDebtAndCapitalLeaseObligations (non-current by definition), else
    LongTermDebt (which includes the current portion) minus that portion."""
    direct = idx.first(
        ("LongTermDebtNoncurrent", "LongTermDebtAndCapitalLeaseObligations"),
        end,
        "instant",
        frequency,
    )
    if direct is not None:
        return direct
    total = idx.value("LongTermDebt", end, "instant", frequency)
    if total is None:
        return None
    current = idx.value("LongTermDebtCurrent", end, "instant", frequency)
    if current is None:
        return total
    return _derived(
        total.value - current.value,
        "LongTermDebt − LongTermDebtCurrent",
        total,
        current,
    )


# ── Statements ───────────────────────────────────────────────────────────────


@dataclass
class Row:
    key: str
    label: str
    statement: str
    unit: str
    derived: bool
    formula: Optional[str]
    values: List[Optional[Figure]]


def _combine(
    figures: Sequence[Optional[Figure]], fn: Callable[..., float], formula: str
) -> Optional[Figure]:
    if any(f is None for f in figures):
        return None
    try:
        value = fn(*(f.value for f in figures))  # type: ignore[union-attr]
    except ZeroDivisionError:
        return None
    return _derived(value, formula, *figures)  # type: ignore[arg-type]


def _ratio(
    num: Optional[Figure], den: Optional[Figure], formula: str
) -> Optional[Figure]:
    if num is None or den is None or den.value == 0:
        return None
    return _derived(num.value / den.value, formula, num, den)


#: Derived rows: key, label, statement, unit, formula, fn(cells) -> Figure.
_DERIVED: Tuple[Tuple[str, str, str, str, str], ...] = (
    ("short_term_debt", "Short-term debt", "balance", "USD", "See methodology"),
    ("long_term_debt", "Long-term debt", "balance", "USD", "See methodology"),
    ("total_debt", "Total debt", "balance", "USD", "Short-term debt + long-term debt"),
    ("net_debt", "Net debt", "balance", "USD", "Total debt − cash and equivalents"),
    (
        "fcf",
        "Free cash flow",
        "cash_flow",
        "USD",
        "Operating cash flow − capital expenditure",
    ),
    (
        "ebitda",
        "EBITDA",
        "income",
        "USD",
        "Operating income + depreciation and amortization",
    ),
)

RATIOS: Tuple[Tuple[str, str, str], ...] = (
    ("gross_margin", "Gross margin", "Gross profit / revenue"),
    ("operating_margin", "Operating margin", "Operating income / revenue"),
    ("net_margin", "Net margin", "Net income / revenue"),
    ("current_ratio", "Current ratio", "Current assets / current liabilities"),
    ("debt_to_equity", "Debt / equity", "Total debt / shareholders' equity"),
    ("net_debt_to_ebitda", "Net debt / EBITDA", "Net debt / EBITDA"),
    ("interest_coverage", "Interest coverage", "Operating income / interest expense"),
)


@dataclass
class Statements:
    company: db.SecCompany
    frequency: str
    periods: List[date]
    rows: List[Row]
    ratios: List[Row]
    urls: Dict[str, str]
    filings: List[db.SecFiling]


def load_company(ticker: str) -> Tuple[db.SecCompany, FactIndex]:
    company = db.sec_company(ticker)
    if company is None:
        raise CompanyNotFound(ticker)
    return company, FactIndex(db.sec_facts(company.cik, ALL_CONCEPTS))


def statements(ticker: str, frequency: Frequency, n_periods: int) -> Statements:
    company, idx = load_company(ticker)
    periods = idx.period_ends(frequency)[:n_periods]

    cells: Dict[str, List[Optional[Figure]]] = {
        item.key: [
            idx.first(item.candidates, end, item.kind, frequency) for end in periods
        ]
        for item in LINE_ITEMS
    }
    cells["short_term_debt"] = [short_term_debt(idx, e, frequency) for e in periods]
    cells["long_term_debt"] = [long_term_debt(idx, e, frequency) for e in periods]

    def zipped(fn, formula, *keys):
        return [
            _combine([cells[k][i] for k in keys], fn, formula)
            for i in range(len(periods))
        ]

    # Total debt needs both parts, but a company with no short-term debt line
    # simply has none: a missing short-term part counts as zero only when the
    # long-term part exists.
    cells["total_debt"] = [
        (
            None
            if lt is None
            else _derived(
                lt.value + (st.value if st else 0.0),
                "Short-term debt + long-term debt"
                + ("" if st else " (no short-term debt tagged)"),
                *[f for f in (st, lt) if f is not None],
            )
        )
        for st, lt in zip(cells["short_term_debt"], cells["long_term_debt"])
    ]
    cells["net_debt"] = zipped(
        lambda d, c: d - c, "Total debt − cash", "total_debt", "cash"
    )
    cells["fcf"] = zipped(
        lambda o, c: o - c, "Operating cash flow − capex", "cfo", "capex"
    )
    cells["ebitda"] = zipped(
        lambda o, d: o + d, "Operating income + D&A", "operating_income", "d_and_a"
    )

    ratio_cells = {
        "gross_margin": ("gross_profit", "revenue"),
        "operating_margin": ("operating_income", "revenue"),
        "net_margin": ("net_income", "revenue"),
        "current_ratio": ("current_assets", "current_liabilities"),
        "debt_to_equity": ("total_debt", "equity"),
        "net_debt_to_ebitda": ("net_debt", "ebitda"),
        "interest_coverage": ("operating_income", "interest_expense"),
    }

    rows: List[Row] = []
    order = [(i.key, i.label, i.statement, i.unit, False, None) for i in LINE_ITEMS]
    order += [
        (k, label, st, unit, True, formula) for k, label, st, unit, formula in _DERIVED
    ]
    for key, label, st, unit, derived, formula in order:
        rows.append(Row(key, label, st, unit, derived, formula, cells[key]))

    ratios = [
        Row(
            key,
            label,
            "ratio",
            "ratio",
            True,
            formula,
            [_ratio(cells[n][i], cells[d][i], formula) for i in range(len(periods))],
        )
        for (key, label, formula), (n, d) in zip(RATIOS, ratio_cells.values())
    ]

    accessions = {
        f.accession for r in rows for f in r.values if f is not None and f.accession
    } | {
        g.accession
        for r in rows
        for f in r.values
        if f is not None
        for g in f.inputs
        if g.accession
    }
    urls = {
        a: u
        for a, u in db.sec_filing_urls(company.cik, sorted(accessions)).items()
        if u
    }
    return Statements(
        company, frequency, periods, rows, ratios, urls, db.sec_filings(company.cik)
    )


# ── What the structural credit model reads ───────────────────────────────────


@dataclass
class BalanceSheetSnapshot:
    period_end: date
    short_term_debt: Optional[Figure]
    long_term_debt: Optional[Figure]
    total_liabilities: Optional[Figure]


def latest_balance_sheet(idx: FactIndex) -> Optional[BalanceSheetSnapshot]:
    """The most recent balance sheet filed (10-K or 10-Q)."""
    ends = idx.period_ends("quarterly")
    if not ends:
        return None
    end = ends[0]
    return BalanceSheetSnapshot(
        end,
        short_term_debt(idx, end, "quarterly"),
        long_term_debt(idx, end, "quarterly"),
        idx.value("Liabilities", end, "instant", "quarterly"),
    )


@dataclass
class SharesOutstanding:
    shares: float
    as_of: date
    concept: str
    filed: date
    accession: str


def shares_outstanding(idx: FactIndex) -> Optional[SharesOutstanding]:
    """The most recent count of shares outstanding: the cover-page figure of a
    report (dei:EntityCommonStockSharesOutstanding) or the balance-sheet one
    (CommonStockSharesOutstanding), whichever is dated later.

    EDGAR's company-facts API carries only facts without an XBRL dimension, so
    a company that reports its count per share class (Alphabet, Berkshire) has
    no cover-page figure here, and the balance-sheet total — every class
    together — is what is read."""
    rows = [
        f for f in idx.facts if f.concept in SHARES_CONCEPTS and f.duration_days == 0
    ]
    if not rows:
        return None
    latest = max(rows, key=lambda f: (f.period_end, f.filed))
    return SharesOutstanding(
        latest.value, latest.period_end, latest.concept, latest.filed, latest.accession
    )
