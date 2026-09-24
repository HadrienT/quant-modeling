"""Interest-rate curves and reference rates by currency — the rates page.

Everything is read from data-ingest's Postgres (`macro.fred_series_latest` for
USD, `macro.intl_rates` for EUR/GBP/CHF/JPY); nothing is fetched live. Two
blocks per currency, kept apart on purpose:

1. **The government curve** — the only complete term structure that is free:
   US Treasury CMT, the ECB's euro-area AAA curve, the Bank of England's gilt
   par yields, Japan's JGB par yields. Quoted as published; where the quotes
   allow it, a zero curve is derived with the C++ bootstrap
   (`qm.bootstrap_discount_curve`) and forwards from that zero curve.
2. **Reference rates** — the overnight benchmark (SOFR, €STR, SONIA, SARON,
   TONA), the policy rate, and the published compounded averages. These are
   fixings, NOT points of a term structure: a "90-day average SOFR" is what
   overnight SOFR compounded to over the PAST 90 days, not a 3-month rate
   going forward. They are listed with their dates, never drawn on a tenor
   axis. OIS swap curves, which would extend them, are not free.

Every rate returned is a decimal (0.0397 for 3.97 %). `METHODOLOGY` below is
the text the page shows: it lives next to the code it describes, so the two
cannot drift apart unnoticed.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field
from datetime import date, timedelta
from typing import Dict, List, Literal, Optional

import quantmodeling as qm

from . import db

Currency = Literal["USD", "EUR", "GBP", "CHF", "JPY"]
CURRENCIES: tuple[str, ...] = ("USD", "EUR", "GBP", "CHF", "JPY")

#: A curve older than this (calendar days) is shown with a staleness warning.
STALE_AFTER_DAYS = 7


@dataclass(frozen=True)
class Pillar:
    tenor: float  # years
    series_id: str
    label: str


@dataclass(frozen=True)
class GovernmentCurve:
    name: str
    table: str  # key of db.RATE_TABLES
    source: str
    source_url: str
    #: How the pillars are quoted: semi-annual par yields, or continuously
    #: compounded zero rates.
    quote: Literal["par_semiannual", "zero_continuous"]
    pillars: tuple[Pillar, ...]
    #: Money-market points quoted as simple rates (US T-bill CMT < 1Y), used
    #: as deposits in front of the par pillars.
    money_market: tuple[Pillar, ...] = ()
    #: Why no zero/forward curve is derived, when none is.
    no_derivation: Optional[str] = None


@dataclass(frozen=True)
class Benchmark:
    series_id: str
    label: str
    kind: Literal["overnight", "policy", "compounded", "interbank_monthly"]
    #: A published average of PAST fixings (not a forward-looking term rate).
    backward_looking: bool = False


@dataclass(frozen=True)
class CurrencyRates:
    currency: str
    table: str
    government: Optional[GovernmentCurve]
    no_government: Optional[str]
    benchmarks: tuple[Benchmark, ...]
    #: The overnight benchmark whose history the page draws.
    headline: str
    notes: tuple[str, ...] = field(default_factory=tuple)


def _p(tenor: float, sid: str, label: str) -> Pillar:
    return Pillar(tenor, sid, label)


def _y(label: str) -> float:
    n, unit = int(label[:-1]), label[-1]
    return n / 12 if unit == "M" else float(n)


CATALOG: Dict[str, CurrencyRates] = {
    "USD": CurrencyRates(
        currency="USD",
        table="fred",
        government=GovernmentCurve(
            name="US Treasury constant-maturity (CMT) par yields",
            table="fred",
            source="U.S. Treasury via FRED (series DGS*)",
            source_url="https://fred.stlouisfed.org/categories/115",
            quote="par_semiannual",
            money_market=(
                _p(1 / 12, "DGS1MO", "1M"),
                _p(0.25, "DGS3MO", "3M"),
                _p(0.5, "DGS6MO", "6M"),
            ),
            pillars=tuple(
                _p(_y(t), f"DGS{t[:-1]}", t)
                for t in ("1Y", "2Y", "3Y", "5Y", "7Y", "10Y", "20Y", "30Y")
            ),
        ),
        no_government=None,
        benchmarks=(
            Benchmark("SOFR", "SOFR (secured overnight financing rate)", "overnight"),
            Benchmark("EFFR", "Effective federal funds rate", "overnight"),
            Benchmark(
                "SOFR30DAYAVG", "SOFR, 30-day compounded average", "compounded", True
            ),
            Benchmark(
                "SOFR90DAYAVG", "SOFR, 90-day compounded average", "compounded", True
            ),
            Benchmark(
                "SOFR180DAYAVG", "SOFR, 180-day compounded average", "compounded", True
            ),
        ),
        headline="SOFR",
    ),
    "EUR": CurrencyRates(
        currency="EUR",
        table="intl",
        government=GovernmentCurve(
            name="Euro area AAA government zero-coupon curve (ECB, Svensson)",
            table="intl",
            source="European Central Bank Data Portal (dataset YC)",
            source_url="https://data.ecb.europa.eu/data/datasets/YC",
            quote="zero_continuous",
            pillars=tuple(
                _p(_y(t), f"EUR.AAA_SPOT_{t}", t)
                for t in (
                    "3M",
                    "6M",
                    "1Y",
                    "2Y",
                    "3Y",
                    "5Y",
                    "7Y",
                    "10Y",
                    "15Y",
                    "20Y",
                    "30Y",
                )
            ),
        ),
        no_government=None,
        benchmarks=(
            Benchmark("EUR.ESTR", "€STR (euro short-term rate)", "overnight"),
            Benchmark("EUR.ECB_DFR", "ECB deposit facility rate", "policy"),
            *(
                Benchmark(
                    f"EUR.ESTR_CA_{t}",
                    f"€STR, {t} compounded average",
                    "compounded",
                    True,
                )
                for t in ("1W", "1M", "3M", "6M", "12M")
            ),
            *(
                Benchmark(
                    f"EUR.EURIBOR_{t}_MONTHLY",
                    f"Euribor {t}, monthly average of daily fixings",
                    "interbank_monthly",
                )
                for t in ("1M", "3M", "6M", "12M")
            ),
        ),
        headline="EUR.ESTR",
    ),
    "GBP": CurrencyRates(
        currency="GBP",
        table="intl",
        government=GovernmentCurve(
            name="UK nominal gilt par yields (Bank of England curve)",
            table="intl",
            source="Bank of England Statistical Interactive Database",
            source_url="https://www.bankofengland.co.uk/boeapps/database/",
            quote="par_semiannual",
            pillars=(
                _p(5.0, "GBP.GILT_PAR_5Y", "5Y"),
                _p(10.0, "GBP.GILT_PAR_10Y", "10Y"),
                _p(20.0, "GBP.GILT_PAR_20Y", "20Y"),
            ),
            no_derivation=(
                "No zero or forward curve is derived for GBP: the free series give "
                "only three par yields (5, 10, 20 years) and nothing shorter, so a "
                "bootstrap would have to invent the whole curve below 5 years."
            ),
        ),
        no_government=None,
        benchmarks=(
            Benchmark(
                "GBP.SONIA", "SONIA (sterling overnight index average)", "overnight"
            ),
            Benchmark("GBP.BANK_RATE", "Bank of England Bank Rate", "policy"),
        ),
        headline="GBP.SONIA",
    ),
    "CHF": CurrencyRates(
        currency="CHF",
        table="intl",
        government=None,
        no_government=(
            "No free CHF government curve: the Swiss National Bank stopped "
            "publishing Swiss Confederation bond yields in 2025 (last data July "
            "2025), and SARON swap rates are licensed data."
        ),
        benchmarks=(
            Benchmark("CHF.SARON", "SARON (Swiss average rate overnight)", "overnight"),
            *(
                Benchmark(
                    f"CHF.SARON_CR_{t}", f"SARON, {t} compound rate", "compounded", True
                )
                for t in ("1M", "3M", "6M")
            ),
        ),
        headline="CHF.SARON",
    ),
    "JPY": CurrencyRates(
        currency="JPY",
        table="intl",
        government=GovernmentCurve(
            name="Japanese government bond (JGB) par yields",
            table="intl",
            source="Ministry of Finance, Japan",
            source_url="https://www.mof.go.jp/english/policy/jgbs/reference/interest_rate/",
            quote="par_semiannual",
            pillars=tuple(
                _p(_y(t), f"JPY.JGB_{t}", t)
                for t in (
                    "1Y",
                    "2Y",
                    "3Y",
                    "4Y",
                    "5Y",
                    "6Y",
                    "7Y",
                    "8Y",
                    "9Y",
                    "10Y",
                    "15Y",
                    "20Y",
                    "25Y",
                    "30Y",
                    "40Y",
                )
            ),
        ),
        no_government=None,
        benchmarks=(
            Benchmark(
                "JPY.TONA", "TONA (uncollateralised overnight call rate)", "overnight"
            ),
        ),
        headline="JPY.TONA",
    ),
}


# ── Computation ──────────────────────────────────────────────────────────────


@dataclass
class CurvePoint:
    tenor: float
    rate: float


@dataclass
class QuotedPoint:
    tenor: float
    label: str
    series_id: str
    rate: float


@dataclass
class CurveResult:
    as_of: date
    quoted: List[QuotedPoint]
    zero: Optional[List[CurvePoint]]
    forward: Optional[List[CurvePoint]]


class RatesUnavailable(RuntimeError):
    """The store holds no complete snapshot of a curve."""


def _grid(start: float, end: float, step: float) -> List[float]:
    """Pillar-independent sampling for the derived curves: every `step` years
    from `start` to `end`, both included."""
    n = int(math.floor((end - start) / step + 1e-9))
    points = [start + i * step for i in range(n + 1)]
    if end - points[-1] > 1e-9:
        points.append(end)
    return points


def discount_curve(curve: GovernmentCurve, quoted: Dict[str, float]):
    """(times, discount factors) at the pillars, from quotes in decimals."""
    if curve.quote == "zero_continuous":
        times = [p.tenor for p in curve.pillars]
        return times, [math.exp(-quoted[p.series_id] * p.tenor) for p in curve.pillars]
    boot = qm.bootstrap_discount_curve(
        [(p.tenor, quoted[p.series_id]) for p in curve.money_market],
        [(p.tenor, quoted[p.series_id]) for p in curve.pillars],
    )
    return list(boot["times"]), list(boot["discount_factors"])


def derived_curves(
    times: List[float], dfs: List[float], forward_period: float
) -> tuple[List[CurvePoint], List[CurvePoint]]:
    """Zero rates (continuous) and Δ-forwards (continuous), both sampled on a
    regular grid from the first to the last pillar with the discount curve's
    own interpolation — so what is drawn between pillars is the curve that was
    built, not a plotting library's spline. Nothing is extrapolated: DiscountCurve
    is flat before the first pillar, which would read as a fake short end."""
    first, last = times[0], times[-1]
    step = 1 / 12 if last <= 2 else 0.25
    grid = sorted(set(_grid(first, last, step)) | set(times))
    zero = [
        CurvePoint(t, -math.log(df) / t)
        for t, df in zip(grid, qm.discount_factors(times, dfs, grid))
    ]
    starts = [t for t in grid if t + forward_period <= last + 1e-9]
    ends = [t + forward_period for t in starts]
    df_start = qm.discount_factors(times, dfs, starts)
    df_end = qm.discount_factors(times, dfs, ends)
    forward = [
        CurvePoint(t, math.log(a / b) / forward_period)
        for t, a, b in zip(starts, df_start, df_end)
    ]
    return zero, forward


def government_curve(curve: GovernmentCurve, forward_period: float) -> CurveResult:
    series = [p.series_id for p in curve.money_market + curve.pillars]
    snapshot = db.rates_curve_snapshot(curve.table, series)
    if snapshot is None:
        raise RatesUnavailable(
            f"{curve.name}: no date on which every pillar is in the store "
            "(run data-ingest's fred-macro / intl-rates source)"
        )
    as_of, values_pct = snapshot
    quoted = {sid: v / 100.0 for sid, v in values_pct.items()}
    quoted_points = [
        QuotedPoint(p.tenor, p.label, p.series_id, quoted[p.series_id])
        for p in sorted(curve.money_market + curve.pillars, key=lambda p: p.tenor)
    ]
    if curve.no_derivation:
        return CurveResult(as_of, quoted_points, None, None)
    times, dfs = discount_curve(curve, quoted)
    zero, forward = derived_curves(times, dfs, forward_period)
    return CurveResult(as_of, quoted_points, zero, forward)


@dataclass
class BenchmarkValue:
    series_id: str
    label: str
    kind: str
    backward_looking: bool
    rate: Optional[float]
    as_of: Optional[date]


def benchmarks(rates: CurrencyRates) -> List[BenchmarkValue]:
    latest = db.rates_latest(rates.table, [b.series_id for b in rates.benchmarks])
    out = []
    for b in rates.benchmarks:
        got = latest.get(b.series_id)
        out.append(
            BenchmarkValue(
                b.series_id,
                b.label,
                b.kind,
                b.backward_looking,
                got[1] / 100.0 if got else None,
                got[0] if got else None,
            )
        )
    return out


def history(currency: str, series_id: str, years: int) -> List[tuple[date, float]]:
    rates = CATALOG[currency]
    allowed = {b.series_id for b in rates.benchmarks}
    if series_id not in allowed:
        raise KeyError(series_id)
    since = date.today() - timedelta(days=int(365.25 * years))
    return [(d, v / 100.0) for d, v in db.rates_history(rates.table, series_id, since)]


def staleness_warning(as_of: date, today: Optional[date] = None) -> Optional[str]:
    age = ((today or date.today()) - as_of).days
    if age > STALE_AFTER_DAYS:
        return (
            f"The latest complete curve is from {as_of.isoformat()}, {age} days ago: "
            "the source has not published since, or data-ingest has not run."
        )
    return None


# ── Methodology (shown on the page) ──────────────────────────────────────────

_COMMON = [
    (
        "Data",
        [
            "Every figure is read from the project's own database, filled daily by "
            "data-ingest from the publishers themselves: FRED for USD, and for the "
            "other currencies the European Central Bank, the Bank of England, the "
            "Swiss National Bank, the Bank of Japan and Japan's Ministry of Finance. "
            "Nothing is fetched live and nothing is estimated when a value is missing.",
            "A curve is always shown for a single market date: the latest date on which "
            "every one of its pillars was published. Pillars from different days are "
            "never spliced together.",
        ],
    ),
    (
        "Reference rates are not a curve",
        [
            "Overnight benchmarks (SOFR, €STR, SONIA, SARON, TONA) and policy rates are "
            "fixings for one day. Their published averages (30/90/180-day SOFR, "
            "compounded €STR, SARON compound rates) are backward-looking: what the "
            "overnight rate compounded to over the past period, not a rate for the "
            "coming period. They are therefore listed with their dates and never drawn "
            "on a maturity axis.",
            "The forward-looking term structure of these benchmarks is the OIS swap "
            "curve, which is not published for free in any currency; neither are daily "
            "Euribor fixings (only their monthly average is, from the ECB).",
        ],
    ),
    (
        "Derived zero and forward curves",
        [
            "Par-yield curves (US Treasury CMT, JGB) are bootstrapped into discount "
            "factors by the library's C++ bootstrap: each maturity is a bond paying a "
            "semi-annual coupon equal to its yield and priced at par, solved one pillar "
            "at a time against the curve already built. US T-bill yields below one year "
            "enter as simple-rate deposits (maturities 1, 3 and 6 months).",
            "The ECB curve is already a zero-coupon curve (Svensson model, continuous "
            "compounding) and is used as published.",
            "Between pillars the discount factor is interpolated log-linearly (piecewise "
            "constant instantaneous forward rate) — the rule the bootstrap itself solves "
            "under. Zero rates are continuously compounded, z(t) = −ln DF(t) / t. The "
            "forward over Δ starting at t is continuously compounded, "
            "f(t, t+Δ) = ln(DF(t) / DF(t+Δ)) / Δ. Nothing is drawn before the first "
            "pillar or after the last one.",
            "A consequence worth reading the chart with: with this interpolation the "
            "forward curve is flat wherever a forward period lies between two pillars "
            "and steps where it crosses one — the long flat stretches between widely "
            "spaced maturities (10Y to 20Y, say) are the interpolation, not the "
            "market. A smoother scheme (monotone convex, Hagan-West) would spread "
            "them; it is not used, so that zero rates, forwards and the bootstrap "
            "share one rule.",
        ],
    ),
]

_PER_CURRENCY = {
    "USD": [
        "Quoted: Treasury constant-maturity yields, 1M to 30Y, which the Treasury "
        "defines as bond-equivalent par yields. The 1M/3M/6M points are bill yields, "
        "used as simple rates.",
    ],
    "EUR": [
        "Quoted: ECB zero-coupon spot rates of the euro-area AAA government curve, "
        "3M to 30Y, continuously compounded. Euribor is shown only as the ECB's "
        "monthly average of the daily fixings, dated the first of its month.",
    ],
    "GBP": [
        "Quoted: nominal gilt par yields at 5, 10 and 20 years from the Bank of "
        "England's fitted curve. Only these three are free, with nothing shorter, "
        "so no zero or forward curve is derived.",
    ],
    "CHF": [
        "No government curve: the SNB stopped publishing Swiss Confederation bond "
        "yields in 2025. SARON and its compound rates are published by the SNB with "
        "a lag of about a week.",
    ],
    "JPY": [
        "Quoted: JGB par yields, 1Y to 40Y, from Japan's Ministry of Finance, "
        "bootstrapped as semi-annual par bonds. TONA is published by the Bank of "
        "Japan with a lag of a few business days.",
    ],
}


def first_pillar_bias_bp(par_yield: float) -> float:
    """How much the library's flat-before-the-first-pillar convention lowers the
    first zero rate when that pillar is a semi-annual par bond with no pillar
    before it (JGB 1Y): the coupon at 6 months is discounted with the 1Y factor.
    Flat: DF = 1/(1+y), z = ln(1+y). Log-linear from the origin: z = 2 ln(1+y/2).
    The gap is ln(1 + y²/4 / (1+y)) ≈ y²/4 — in basis points."""
    return math.log(1 + par_yield**2 / 4 / (1 + par_yield)) * 1e4


def methodology(
    currency: str, result: Optional[CurveResult] = None
) -> List[tuple[str, List[str]]]:
    specifics = list(_PER_CURRENCY[currency])
    curve = CATALOG[currency].government
    if (
        result is not None
        and result.zero is not None
        and curve is not None
        and curve.quote == "par_semiannual"
        and not curve.money_market
    ):
        first = min(result.quoted, key=lambda q: q.tenor)
        specifics.append(
            f"The first pillar ({first.label}) is a par bond with a coupon before it "
            "and no pillar there: the library holds the discount factor flat before "
            "its first pillar, which lowers that zero rate by ln(1 + y²/4 / (1+y)) "
            f"≈ y²/4 — {first_pillar_bias_bp(first.rate):.2f} bp for today's "
            f"{first.rate * 100:.3f} % par yield."
        )
    return _COMMON + [(f"{currency} specifics", specifics)]
