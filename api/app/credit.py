"""Credit curves and structural credit risk — the credit page.

Two views of US corporate credit, kept apart because they answer different
questions and neither is a CDS quote (single-name CDS quotes are not free):

1. **Market spreads** — the ICE BofA US corporate bond indices' option-adjusted
   spreads, from FRED via data-ingest: by rating (AAA → CCC and lower) and,
   for investment grade as a whole, by maturity bucket (1-3Y → 15Y+). The
   maturity buckets are the only free credit term structure; they are
   bootstrapped by the C++ library (`qm.bootstrap_credit_curve`) into a
   piecewise-constant hazard rate, read as if each bucket's OAS were a par
   CDS spread. Each rating becomes a flat hazard rate the same way.
2. **Structural (Merton 1974)** — for one company, the asset value and asset
   volatility implied by its equity (market capitalisation and realised
   volatility) and its debt (10-K / 10-Q), and the credit spread and default
   probability the model then gives at each maturity.

Every rate, spread and probability returned is a decimal. `METHODOLOGY` is the
text the page shows, next to the code it describes.
"""

from __future__ import annotations

import math
from dataclasses import dataclass
from datetime import date, timedelta
from typing import Dict, List, Optional, Tuple

import quantmodeling as qm

from . import db, fundamentals, rates

#: A spread snapshot older than this (calendar days) carries a warning.
STALE_AFTER_DAYS = 7


@dataclass(frozen=True)
class SpreadSeries:
    series_id: str
    label: str
    group: str  # "IG" | "HY" | "maturity" | "aggregate"
    #: Where the bucket sits on the maturity axis (maturity buckets only).
    tenor: Optional[float] = None


MATURITY_BUCKETS: Tuple[SpreadSeries, ...] = (
    SpreadSeries("BAMLC1A0C13Y", "1-3Y", "maturity", 2.0),
    SpreadSeries("BAMLC2A0C35Y", "3-5Y", "maturity", 4.0),
    SpreadSeries("BAMLC3A0C57Y", "5-7Y", "maturity", 6.0),
    SpreadSeries("BAMLC4A0C710Y", "7-10Y", "maturity", 8.5),
    SpreadSeries("BAMLC7A0C1015Y", "10-15Y", "maturity", 12.5),
    SpreadSeries("BAMLC8A0C15PY", "15Y+", "maturity", 20.0),
)

RATINGS: Tuple[SpreadSeries, ...] = (
    SpreadSeries("BAMLC0A1CAAA", "AAA", "IG"),
    SpreadSeries("BAMLC0A2CAA", "AA", "IG"),
    SpreadSeries("BAMLC0A3CA", "A", "IG"),
    SpreadSeries("BAMLC0A4CBBB", "BBB", "IG"),
    SpreadSeries("BAMLH0A1HYBB", "BB", "HY"),
    SpreadSeries("BAMLH0A2HYB", "B", "HY"),
    SpreadSeries("BAMLH0A3HYC", "CCC and lower", "HY"),
)

AGGREGATES: Tuple[SpreadSeries, ...] = (
    SpreadSeries("BAMLC0A0CM", "US investment grade", "aggregate"),
    SpreadSeries("BAMLH0A0HYM2", "US high yield", "aggregate"),
)

#: Moody's seasoned corporate yields (not spreads): decades of daily history.
REFERENCE_YIELDS: Tuple[SpreadSeries, ...] = (
    SpreadSeries("DAAA", "Moody's Aaa corporate yield", "yield"),
    SpreadSeries("DBAA", "Moody's Baa corporate yield", "yield"),
)

HISTORY_SERIES: Dict[str, SpreadSeries] = {
    s.series_id: s for s in MATURITY_BUCKETS + RATINGS + AGGREGATES + REFERENCE_YIELDS
}

#: The flat hazard of a rating is solved for a CDS of this maturity.
RATING_HAZARD_MATURITY = 5.0
#: Default-probability horizons shown per rating.
PD_HORIZONS = (1.0, 5.0, 10.0)
#: Maturities of the structural term structure.
STRUCTURAL_MATURITIES = (0.5, 1.0, 2.0, 3.0, 5.0, 7.0, 10.0)
#: KMV's convention: default point = short-term debt + half the long-term debt.
LONG_TERM_DEBT_WEIGHT = 0.5
#: Trading days of daily returns behind the realised equity volatility.
VOL_WINDOW = 252


class CreditUnavailable(RuntimeError):
    """A curve or a model that cannot be built from what is in the store."""


def staleness_warning(
    as_of: date, what: str, today: Optional[date] = None
) -> Optional[str]:
    age = ((today or date.today()) - as_of).days
    if age > STALE_AFTER_DAYS:
        return (
            f"The latest {what} is from {as_of.isoformat()}, {age} days ago: FRED has "
            "not published since, or data-ingest has not run."
        )
    return None


def _usd_discount() -> rates.DiscountCurveSnapshot:
    try:
        return rates.currency_discount_curve("USD")
    except rates.RatesUnavailable as exc:
        raise CreditUnavailable(f"No USD discount curve: {exc}") from exc


# ── Market spreads ───────────────────────────────────────────────────────────


@dataclass
class QuotedSpread:
    series_id: str
    label: str
    tenor: Optional[float]
    spread: float


@dataclass
class HazardCurve:
    times: List[float]
    hazards: List[float]
    #: Survival and cumulative default probability on a regular grid.
    grid: List[float]
    survival: List[float]
    #: Largest |model par spread − quote| over the inputs, in basis points:
    #: the bootstrap's own check, shown on the page.
    max_repricing_error_bp: float


@dataclass
class RatingRow:
    series_id: str
    label: str
    group: str
    spread: float
    hazard: float
    default_probabilities: List[float]  # at PD_HORIZONS


@dataclass
class SpreadsOverview:
    as_of: date
    recovery: float
    discount_as_of: date
    term_structure: List[QuotedSpread]
    hazard: Optional[HazardCurve]
    hazard_unavailable: Optional[str]
    ratings_as_of: Optional[date]
    ratings: List[RatingRow]
    aggregates: List[Tuple[SpreadSeries, Optional[date], Optional[float]]]
    reference_yields: List[Tuple[SpreadSeries, Optional[date], Optional[float]]]
    warnings: List[str]


def _grid(end: float, step: float = 0.25) -> List[float]:
    return [round(i * step, 10) for i in range(int(round(end / step)) + 1)]


def hazard_curve(
    quotes: List[QuotedSpread], disc: rates.DiscountCurveSnapshot, recovery: float
) -> HazardCurve:
    boot = qm.bootstrap_credit_curve(
        [(q.tenor, q.spread) for q in quotes], disc.times, disc.dfs, recovery
    )
    times, hazards = list(boot["times"]), list(boot["hazards"])
    worst = 0.0
    for q in quotes:
        legs = qm.cds_legs(
            q.tenor, q.spread, times, hazards, disc.times, disc.dfs, recovery
        )
        worst = max(worst, abs(legs["par_spread"] - q.spread) * 1e4)
    grid = _grid(max(times))
    return HazardCurve(
        times,
        hazards,
        grid,
        list(qm.survival_probabilities(times, hazards, grid)),
        worst,
    )


def spreads_overview(recovery: float) -> SpreadsOverview:
    warnings: List[str] = []
    disc = _usd_discount()

    snap = db.rates_curve_snapshot("fred", [b.series_id for b in MATURITY_BUCKETS])
    if snap is None:
        raise CreditUnavailable(
            "No date on which every ICE BofA maturity bucket is in the store "
            "(run data-ingest's fred-macro source)"
        )
    as_of, values = snap
    term = [
        QuotedSpread(b.series_id, b.label, b.tenor, values[b.series_id] / 100.0)
        for b in MATURITY_BUCKETS
    ]
    stale = staleness_warning(as_of, "spread curve")
    if stale:
        warnings.append(stale)

    hazard, unavailable = None, None
    try:
        hazard = hazard_curve(term, disc, recovery)
    except RuntimeError as exc:  # the C++ bootstrap refusing an inverted curve
        unavailable = f"The hazard curve could not be bootstrapped: {exc}"
        warnings.append(unavailable)

    ratings: List[RatingRow] = []
    rsnap = db.rates_curve_snapshot("fred", [r.series_id for r in RATINGS])
    ratings_as_of = None
    if rsnap is None:
        warnings.append("Spreads by rating are not in the store yet.")
    else:
        ratings_as_of, rvalues = rsnap
        for r in RATINGS:
            spread = rvalues[r.series_id] / 100.0
            boot = qm.bootstrap_credit_curve(
                [(RATING_HAZARD_MATURITY, spread)], disc.times, disc.dfs, recovery
            )
            lam = boot["hazards"][0]
            ratings.append(
                RatingRow(
                    r.series_id,
                    r.label,
                    r.group,
                    spread,
                    lam,
                    [1.0 - math.exp(-lam * t) for t in PD_HORIZONS],
                )
            )

    latest = db.rates_latest(
        "fred", [s.series_id for s in AGGREGATES + REFERENCE_YIELDS]
    )

    def pick(series):
        out = []
        for s in series:
            got = latest.get(s.series_id)
            out.append((s, got[0] if got else None, got[1] / 100.0 if got else None))
        return out

    return SpreadsOverview(
        as_of,
        recovery,
        disc.as_of,
        term,
        hazard,
        unavailable,
        ratings_as_of,
        ratings,
        pick(AGGREGATES),
        pick(REFERENCE_YIELDS),
        warnings,
    )


def spread_history(series_id: str, years: int) -> List[Tuple[date, float]]:
    if series_id not in HISTORY_SERIES:
        raise KeyError(series_id)
    since = date.today() - timedelta(days=int(365.25 * years))
    return [(d, v / 100.0) for d, v in db.rates_history("fred", series_id, since)]


# ── Structural model, per company ────────────────────────────────────────────


@dataclass
class StructuralInputs:
    price: float
    price_date: date
    shares: fundamentals.SharesOutstanding
    equity_value: float
    equity_vol: float
    vol_observations: int
    balance_sheet: fundamentals.BalanceSheetSnapshot
    default_point: float
    rate: float


@dataclass
class StructuralResult:
    company: db.SecCompany
    ticker: str
    inputs: Optional[StructuralInputs]
    unavailable: Optional[str]
    asset_value: Optional[float] = None
    asset_vol: Optional[float] = None
    leverage: Optional[float] = None  # default point / asset value
    converged: Optional[bool] = None
    iterations: Optional[int] = None
    maturities: Optional[List[float]] = None
    spreads: Optional[List[float]] = None
    default_probabilities: Optional[List[float]] = None
    distances_to_default: Optional[List[float]] = None
    expected_recoveries: Optional[List[float]] = None
    warnings: Optional[List[str]] = None


def is_financial(sic: Optional[str]) -> bool:
    """SIC 6000–6799: banks, insurers, brokers, REITs and other financials."""
    try:
        return 6000 <= int(sic or "") <= 6799
    except ValueError:
        return False


def realised_vol(ticker: str) -> Tuple[float, int, date, float]:
    """(annualised vol, returns used, last date, last close) over the last
    VOL_WINDOW daily log returns."""
    closes = db.price_history(ticker, date.today() - timedelta(days=500))
    closes = closes[-(VOL_WINDOW + 1) :]
    if len(closes) < 60:
        raise CreditUnavailable(f"Not enough daily closes for {ticker} in the store.")
    rets = [math.log(b[1] / a[1]) for a, b in zip(closes, closes[1:])]
    mean = sum(rets) / len(rets)
    var = sum((r - mean) ** 2 for r in rets) / (len(rets) - 1)
    return math.sqrt(var * 252.0), len(rets), closes[-1][0], closes[-1][1]


def structural(ticker: str) -> StructuralResult:
    company, idx = fundamentals.load_company(ticker)
    ticker = ticker.upper()
    warnings: List[str] = []

    def unavailable(reason: str, inputs=None) -> StructuralResult:
        return StructuralResult(company, ticker, inputs, reason, warnings=warnings)

    if is_financial(company.sic):
        return unavailable(
            f"{company.name} is a financial company (SIC {company.sic}, "
            f"{company.sic_description}). Its liabilities are mostly deposits, policy "
            "reserves or trading positions, not the debt Merton's model treats as a "
            "default barrier: the model is not applied."
        )

    bs = fundamentals.latest_balance_sheet(idx)
    shares = fundamentals.shares_outstanding(idx)
    if bs is None or shares is None:
        return unavailable(
            "No balance sheet or share count in the store for this company."
        )
    std = bs.short_term_debt.value if bs.short_term_debt else 0.0
    ltd = bs.long_term_debt.value if bs.long_term_debt else 0.0
    default_point = std + LONG_TERM_DEBT_WEIGHT * ltd
    if bs.short_term_debt is None and bs.long_term_debt is None:
        return unavailable(
            f"No debt found on the {bs.period_end.isoformat()} balance sheet in the "
            "standard us-gaap concepts this page reads (some companies tag their debt "
            "with their own XBRL extensions): the model has no default barrier."
        )
    if default_point <= 0:
        return unavailable("The default point (short-term + ½ long-term debt) is zero.")

    try:
        vol, n, price_date, price = realised_vol(ticker)
    except CreditUnavailable as exc:
        return unavailable(str(exc))

    disc = _usd_discount()
    rate = -math.log(qm.discount_factors(disc.times, disc.dfs, [1.0])[0])

    if (price_date - shares.as_of).days > 200:
        warnings.append(
            f"The share count is from {shares.as_of.isoformat()}, "
            f"{(price_date - shares.as_of).days} days before the price."
        )
    if len(company.tickers) > 1:
        warnings.append(
            f"{company.name} has several listed share classes ({', '.join(company.tickers)}); "
            f"every share is valued at the {ticker} price."
        )
    equity = price * shares.shares
    inputs = StructuralInputs(
        price, price_date, shares, equity, vol, n, bs, default_point, rate
    )

    cal = qm.merton_calibrate(equity, vol, default_point, rate, 1.0)
    if not cal["converged"]:
        return unavailable("The Merton calibration did not converge.", inputs)
    ts = qm.merton_term_structure(
        cal["asset_value"],
        cal["asset_vol"],
        default_point,
        rate,
        list(STRUCTURAL_MATURITIES),
    )
    return StructuralResult(
        company,
        ticker,
        inputs,
        None,
        asset_value=cal["asset_value"],
        asset_vol=cal["asset_vol"],
        leverage=default_point / cal["asset_value"],
        converged=cal["converged"],
        iterations=cal["iterations"],
        maturities=list(ts["maturities"]),
        spreads=[max(0.0, s) for s in ts["credit_spread"]],
        default_probabilities=list(ts["default_probability"]),
        distances_to_default=list(ts["distance_to_default"]),
        expected_recoveries=list(ts["expected_recovery"]),
        warnings=warnings,
    )


# ── Methodology (shown on the page) ──────────────────────────────────────────

SPREADS_METHODOLOGY: List[Tuple[str, List[str]]] = [
    (
        "Data",
        [
            "Spreads are the option-adjusted spreads (OAS) of the ICE BofA US corporate "
            "bond indices, published daily on FRED and stored by data-ingest; nothing is "
            "fetched live. ICE allows FRED to serve only the last three years of them. "
            "Moody's seasoned Aaa and Baa yields, also from FRED, go back decades.",
            "Single-name CDS quotes (Markit, ICE, Bloomberg) are not free and are not "
            "shown. What is shown are bond spreads, which differ from CDS spreads by the "
            "CDS–bond basis, and which carry a liquidity and tax component besides "
            "default risk (Elton, Gruber, Agrawal & Mann 2001; Longstaff, Mithal & "
            "Neis 2005).",
        ],
    ),
    (
        "From spreads to a hazard curve",
        [
            "Only investment grade as a whole is published by maturity: six buckets, "
            "1-3Y to 15Y+. Each bucket is placed at its midpoint (2, 4, 6, 8.5 and "
            "12.5 years), and 15Y+ at 20 years: ICE's average maturity per bucket is not "
            "free, so these pillar placements are a convention, not market data.",
            "Each bucket's OAS is then read as the par spread of a CDS of that maturity "
            "paying quarterly, and the library's C++ bootstrap solves one hazard rate per "
            "bucket, constant between pillars, so that every such CDS is worth zero — the "
            "ISDA standard model's construction (O'Kane 2008, ch. 6-7; Hull & White "
            "2000). The legs are integrated exactly between the knots of the hazard curve, "
            "the discount curve and the payment dates, with premium accrued on default. "
            "The largest repricing error of the inputs is shown next to the curve.",
            "Discounting uses the US Treasury curve of the rates page. Recovery is an "
            "assumption (40 % is the market convention for senior unsecured debt), "
            "adjustable: the hazard rate scales roughly as spread / (1 − recovery), the "
            '"credit triangle".',
            "Survival S(t) = exp(−∫λ) and default probability 1 − S(t) are "
            "risk-neutral: they price the spread, including whatever part of it is not "
            "default risk, and are larger than historical default frequencies.",
        ],
    ),
    (
        "By rating",
        [
            "The rating indices have no term structure: each OAS is turned into a single "
            "flat hazard rate by the same bootstrap on a 5-year CDS, and default "
            "probabilities at 1, 5 and 10 years follow as 1 − exp(−λt). A flat hazard "
            "ignores that investment-grade default risk rises with maturity and "
            "high-yield risk falls — which is the point of the maturity view above.",
        ],
    ),
]

STRUCTURAL_METHODOLOGY: List[Tuple[str, List[str]]] = [
    (
        "Merton's model",
        [
            "The firm's assets follow a geometric Brownian motion; the firm owes one "
            "zero-coupon debt and defaults at its maturity if assets fall short of it. "
            "Equity is then a call option on the assets (Merton 1974). From the observed "
            "equity value and equity volatility, the two unobservable inputs — asset "
            "value and asset volatility — are solved jointly by Newton's method on "
            "E = C(V, σ_V) and σ_E·E = N(d₁)·σ_V·V (Jones, Mason & Rosenfeld 1984).",
            "Default point: short-term debt plus half the long-term debt, from the latest "
            "10-K or 10-Q balance sheet — Moody's KMV convention (Crosbie & Bohn 2003). "
            "Operating leases are left out. The calibration horizon is one year and the "
            "riskless rate is the 1-year zero rate of the US Treasury curve.",
            "Equity value: the latest close times the latest share count filed. Equity "
            "volatility: the realised volatility of the last 252 daily log returns, "
            "annualised by √252 — backward-looking, where an option-implied volatility "
            "would be forward-looking.",
        ],
    ),
    (
        "Reading the results",
        [
            "The term structure keeps the calibrated asset value and volatility and the "
            "same default point for every maturity. The distance to default is d₂, in "
            "standard deviations; the default probability N(−d₂) and the spread are "
            "risk-neutral.",
            "The model's known bias: with a continuous asset path and default only at "
            "maturity, a firm far from its default point has almost no short-dated "
            "spread, and model spreads fall well below market spreads for investment-"
            "grade names (Eom, Helwege & Huang 2004). The market curves are drawn "
            "alongside for that reason: the gap is the model's limitation, not a "
            "mispricing.",
            "Financial companies (SIC 6000–6799) are not modelled: deposits and insurance "
            "reserves are not the debt the model's barrier stands for.",
        ],
    ),
]

FUNDAMENTALS_METHODOLOGY: List[Tuple[str, List[str]]] = [
    (
        "Source",
        [
            "Figures are the XBRL facts of each company's 10-K and 10-Q filings, from SEC "
            "EDGAR, stored weekly by data-ingest for the S&P 500. Every figure links to "
            "the filing it comes from.",
            "A line reads the first of several XBRL concepts the company has tagged, in a "
            "fixed order (for revenue: Revenues, then "
            "RevenueFromContractWithCustomerExcludingAssessedTax, …); the concept used is "
            "shown with each figure. When a later filing restates a period, the latest "
            "figure is shown. Nothing is estimated: an untagged line is empty.",
            "Annual columns are fiscal years (a flow over 350–380 days, the balance sheet "
            "at the fiscal year end); quarterly columns are 80–100-day flows. A fourth "
            "quarter is not filed on a 10-Q and is not derived.",
        ],
    ),
    (
        "Debt",
        [
            "Short-term debt: DebtCurrent when tagged, otherwise the current portion of "
            "long-term debt plus short-term borrowings (or commercial paper, never both, "
            "since many filers report paper inside borrowings). Long-term debt: "
            "LongTermDebtNoncurrent, else LongTermDebtAndCapitalLeaseObligations, else "
            "LongTermDebt minus its current portion. Operating leases are a separate "
            "line, not debt.",
            "EBITDA here is operating income plus depreciation and amortization, both as "
            "tagged — not an adjusted EBITDA.",
        ],
    ),
]
