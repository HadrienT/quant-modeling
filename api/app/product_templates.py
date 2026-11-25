"""Term sheet -> payoff script: the structured products of the library as
products with named terms (blueprint/wp/16-scripting.md §8.8).

Each product is one readable script in product_library/*.qms, opening with a
header the site reads:

    # title: Worst-of Phoenix autocall
    # category: Multi-asset
    # underlyings: 3
    # quanto: USD                      (optional: one underlying listed in
                                       another currency, paid in this one)
    # market: rates                    (optional: no stock -- priced on the
                                       government curve, by Hull-White)
    # eps: 1                           (optional: the smoothing half-width of
                                       its adjoint greeks, measured -- WP 26,
                                       qm_gpu_fuzzy_eps_bench)
    # greeks: separate                 (with eps: "joint", one smoothed run
                                       for the price and the greeks; or
                                       "separate", the price unsmoothed and the
                                       greeks from a second, smoothed run; or
                                       "last-step", for a script on
                                       lastStepDigital: its own greeks, except
                                       under the models where they fail
                                       (LAST_STEP_FALLBACK_MODELS), which take
                                       them from the threshold smoothed at eps;
                                       or "knock-out", for a script on
                                       knockOutAbove / Below: the same, the
                                       thresholds the knock-outs stand for)
    # curve: sofr                      (optional, rates only: the USD SOFR
                                       curve of the traded swaps, Hull-White
                                       calibrated to the traded swaptions)
    # source: ...                      (one line per source, at least one)
    # summary: ...
    # param: coupon = 0.025 | percent | Quarterly coupon, % of notional

Every `param` is assigned at the script's first event (`    coupon = 0.025`)
and used by name below, so a term sheet only replaces those numbers: the
script priced is the file, with the investor's terms. Units: `percent` is a
fraction (0.025 = 2.5 %), `amount` a cash amount, `number` a plain number.

Dates: a template is written on fixed dates; render() moves every date by the
same whole number of weeks so that the first one falls a week after the
valuation date (weekdays, and so business days and schedules, are kept).

The same files feed the scripting page's library and the pricing page's
catalog through web/src/shared/products/scripted.gen.json
(scripts/gen_product_library.py, checked by CI).
"""

from __future__ import annotations

import math
import re
from dataclasses import dataclass, field
from datetime import date, timedelta
from functools import lru_cache
from pathlib import Path
from typing import Dict, List, Mapping

LIBRARY = Path(__file__).with_name("product_library")
LEAD_DAYS = 7
UNITS = ("percent", "amount", "number", "side")

_META = re.compile(
    r"^# (title|category|underlyings|quanto|market|curve|eps|greeks|source|summary): (.+)$"
)
_PARAM = re.compile(r"^# param: (\w+) = (\S+) \| (\w+) \| (.+)$")
_ISO = re.compile(r"\b\d{4}-\d{2}-\d{2}\b")


@dataclass(frozen=True)
class Param:
    name: str
    default: float
    unit: str
    label: str


@dataclass(frozen=True)
class Template:
    slug: str
    title: str
    category: str
    underlyings: int
    sources: tuple
    summary: str
    params: tuple
    #: The file, header included.
    text: str
    #: The file without its metadata lines: what an editor shows.
    script: str = field(repr=False)
    #: Payment currency of a quanto (one underlying listed in another
    #: currency, priced on `underlyings` with `currency`); None otherwise.
    quanto: str | None = None
    #: "rates": the script reads no stock, only the curve (df(), fwdRate());
    #: priced by Hull-White fitted to the currency's government curve.
    market: str = "equity"
    #: The curve of a rates product: "government" (the currency's, chosen
    #: on the request) or "sofr" (USD SOFR from the swaps DTCC published,
    #: Hull-White calibrated to the swaptions traded with them: swaps and
    #: swaptions, rates_derivatives.sofr_hull_white).
    curve: str = "government"
    #: The smoothing half-width of its adjoint greeks (None: none), in the
    #: units of the expressions its conditions compare, and how the price is
    #: taken: "joint" from the smoothed run (its bias measured below half an
    #: error), "separate" from a second, unsmoothed run (WP 26 §5, #226
    #: follow-up), or "last-step" (smoothed_last_step).
    eps: float | None = None
    greeks: str = "joint"
    #: Signed parts (#369 L2b): (+1 or -1, script), the product's price
    #: their signed sum -- a knock-in as the vanilla less the knock-out,
    #: E[1{hit} f] = E[f] - E[1{alive} f], since the paths a knock-out's
    #: conditioned step simulates never touch the barrier. Written after the
    #: script in "# part: +" / "# part: -" sections, the same terms and the
    #: same dates; the Price page's Monte-Carlo prices them (render_parts),
    #: every other use reads `script`, the product as one hard script.
    parts: tuple = ()


_PART = re.compile(r"^# part: ([+-])\s*(.*)$")


def parse(slug: str, text: str) -> Template:
    meta: Dict[str, str] = {}
    sources: List[str] = []
    params: List[Param] = []
    body: List[str] = []
    parts: List[tuple] = []
    for line in text.splitlines():
        m, p = _META.match(line), _PARAM.match(line)
        if (q := _PART.match(line)) is not None:
            parts.append((1 if q.group(1) == "+" else -1, []))
        elif parts:
            parts[-1][1].append(line)
        elif p:
            name, default, unit, label = p.groups()
            if unit not in UNITS:
                raise ValueError(f"{slug}: unknown unit '{unit}' for {name}")
            params.append(Param(name, float(default), unit, label.strip()))
        elif m and m.group(1) == "source":
            sources.append(m.group(2).strip())
        elif m:
            meta[m.group(1)] = m.group(2).strip()
        else:
            body.append(line)
    while body and re.fullmatch(r"#?\s*", body[0]):
        body.pop(0)
    t = Template(
        slug=slug,
        title=meta["title"],
        category=meta["category"],
        underlyings=int(meta["underlyings"]),
        sources=tuple(sources),
        summary=meta["summary"],
        params=tuple(params),
        text=text,
        script="\n".join(body) + "\n",
        quanto=meta.get("quanto"),
        market=meta.get("market", "equity"),
        curve=meta.get("curve", "government"),
        eps=float(meta["eps"]) if "eps" in meta else None,
        greeks=meta.get("greeks", "joint"),
        parts=tuple(
            (sign, "\n".join(lines).strip("\n") + "\n") for sign, lines in parts
        ),
    )
    if t.greeks not in ("joint", "separate", "last-step", "knock-out"):
        raise ValueError(
            f"{slug}: greeks must be 'joint', 'separate', 'last-step' or 'knock-out'"
        )
    if t.greeks == "knock-out" and (t.eps is None or "knockOut" not in t.script):
        raise ValueError(
            f"{slug}: greeks 'knock-out' needs eps and knockOutAbove / knockOutBelow"
        )
    if t.greeks == "last-step" and (t.eps is None or "lastStepDigital" not in t.script):
        raise ValueError(f"{slug}: greeks 'last-step' needs eps and lastStepDigital")
    if t.eps is not None and not t.eps > 0:
        raise ValueError(f"{slug}: eps must be positive")
    span = lambda s: (min(_ISO.findall(s)), max(_ISO.findall(s)))  # noqa: E731
    for sign, part in t.parts:
        # render dates every part from its first date and stretches it to
        # its last: the parts must span the script's dates exactly
        if not _ISO.findall(part) or span(part) != span(t.script):
            raise ValueError(
                f"{slug}: a part must start and end on the script's first and last dates"
            )
    if t.market not in ("equity", "rates"):
        raise ValueError(f"{slug}: unknown market '{t.market}'")
    if t.curve not in ("government", "sofr"):
        raise ValueError(f"{slug}: unknown curve '{t.curve}'")
    if t.curve != "government" and t.market != "rates":
        raise ValueError(f"{slug}: a curve is for a rates product (# market: rates)")
    for prm in t.params:
        if not _assignment(prm.name).search(t.script):
            raise ValueError(f"{slug}: param {prm.name} is never assigned")
    return t


def _assignment(name: str) -> "re.Pattern[str]":
    return re.compile(rf"^(\s+{re.escape(name)} = )(\S+)$", re.M)


@lru_cache(maxsize=1)
def templates() -> Dict[str, Template]:
    return {p.stem: parse(p.stem, p.read_text()) for p in sorted(LIBRARY.glob("*.qms"))}


def get(slug: str) -> Template:
    try:
        return templates()[slug]
    except KeyError:
        raise ValueError(
            f"unknown scripted product '{slug}' (known: {', '.join(templates())})"
        ) from None


def _fmt(x: float) -> str:
    return repr(float(x)) if not float(x).is_integer() else str(int(x))


def align_dates(script: str, valuation_date: date, lead_days: int = LEAD_DAYS) -> str:
    """Moves every date by whole weeks so that the first falls `lead_days`
    to `lead_days` + 6 days after the valuation date (whole weeks keep every
    date on its weekday)."""
    dates = [date.fromisoformat(d) for d in _ISO.findall(script)]
    if not dates:
        return script
    gap = (valuation_date + timedelta(days=lead_days) - min(dates)).days
    weeks = -(-gap // 7)  # ceiling
    if weeks == 0:
        return script
    shift = timedelta(weeks=weeks)
    return _ISO.sub(lambda m: (date.fromisoformat(m[0]) + shift).isoformat(), script)


def start_on(script: str, start: date) -> str:
    """Moves every date by the same number of days so that the first is
    `start` (the Price page's library products start on the valuation date,
    their first fixing today's spot). Dates may then fall on a weekend: the
    simulation runs on calendar time, and a schedule() still adjusts its own
    dates."""
    dates = [date.fromisoformat(d) for d in _ISO.findall(script)]
    if not dates:
        return script
    shift = start - min(dates)
    return _ISO.sub(lambda m: (date.fromisoformat(m[0]) + shift).isoformat(), script)


def _weekday(d: date) -> date:
    """A Saturday or a Sunday rolled to the Monday after."""
    return d + timedelta(days=(7 - d.weekday()) % 7) if d.weekday() >= 5 else d


def span_years(slug: str) -> float:
    """The template's length: its last date from its first, in years."""
    dates = [date.fromisoformat(d) for d in _ISO.findall(get(slug).script)]
    return (max(dates) - min(dates)).days / 365.25


def stretch(script: str, years: float) -> str:
    """Every date moved away from the first in proportion, so that the last
    falls `years` after it; a weekend date rolls to the Monday after. A
    schedule() keeps its frequency (a two-year quarterly autocall gets eight
    observations), a written date keeps its place in the product's life."""
    dates = sorted({date.fromisoformat(d) for d in _ISO.findall(script)})
    if len(dates) < 2 or not years > 0:
        return script
    first, span = dates[0], (dates[-1] - dates[0]).days
    target = round(years * 365.25)
    moved: Dict[date, date] = {}
    last = first
    for d in dates:
        n = _weekday(first + timedelta(days=round((d - first).days * target / span)))
        if d != first and n <= last:  # shrunk onto its predecessor: keep the order
            n = _weekday(last + timedelta(days=1))
        moved[d] = n
        last = n
    return _ISO.sub(lambda m: moved[date.fromisoformat(m[0])].isoformat(), script)


#: Rates products whose dates are their coupons': the script is written for
#: the asked length (and expiry) rather than stretched, every period a year.
RATES_GENERATED = ("interest-rate-swap", "european-swaption", "bermudan-swaption")


def _add_years(d: date, years: float) -> date:
    whole = int(years)
    if abs(years - whole) < 1e-9:
        try:
            return d.replace(year=d.year + whole)
        except ValueError:  # 29 February
            return d.replace(year=d.year + whole, day=28)
    return d + timedelta(days=round(years * 365.25))


def _rates_body(slug: str, first: date, tenor: int, expiry: float) -> str:
    """The script of a swap or a swaption on annual periods, its terms the
    template's assignments (filled by render)."""
    t = get(slug)
    head = [first.isoformat()] + [f"    {p.name} = {_fmt(p.default)}" for p in t.params]
    if slug == "interest-rate-swap":
        d = [_weekday(_add_years(first, i)) for i in range(tenor + 1)]
        out, lines = head, []
        for i in range(tenor):
            s, e = d[i].isoformat(), d[i + 1].isoformat()
            pay = f"    pays side * notional * (fwdRate({s}, {e}) - fixed_rate) * yf({s}, {e}) * df({e})"
            if i == 0:
                out.append(pay)
            else:
                lines += ["", s, pay]
        return "\n".join(out + lines) + "\n"
    x = _weekday(_add_years(first, expiry))
    e = [_weekday(_add_years(x, j)) for j in range(tenor + 1)]
    end = e[-1].isoformat()

    def annuity(k: int) -> str:
        return " + ".join(
            f"yf({e[j - 1].isoformat()}, {e[j].isoformat()}) * df({e[j].isoformat()})"
            for j in range(k + 1, tenor + 1)
        )

    out = head
    if slug == "european-swaption":
        out += [
            "",
            e[0].isoformat(),
            f"    annuity = {annuity(0)}",
            f"    swap = 1 - df({end}) - strike * annuity",
            "    pays notional * max(side * swap, 0)",
        ]
    else:
        for k in range(tenor):
            out += [
                "",
                e[k].isoformat(),
                f"    value = side * (1 - df({end}) - strike * ({annuity(k)}))",
                "    if value > 0 and exercise(value) then",
                "        pays notional * value",
                "    endIf",
            ]
    return "\n".join(out) + "\n"


def render(
    slug: str,
    terms: Mapping[str, float],
    valuation_date: date,
    lead_days: int = LEAD_DAYS,
    maturity: float | None = None,
    expiry: float | None = None,
    start: date | None = None,
) -> str:
    """The product's script with the given terms (the others at their
    defaults), dated to start a week after `valuation_date` -- or, booked in
    a portfolio (lead_days = 0), within the week that starts on the trade
    date; or exactly on `start`.

    `maturity` (years): the product's length -- its dates stretched from the
    first (stretch); for a swap or a swaption, the swap's length in whole
    years, written period by period, and `expiry` the years to the
    swaption's (first) exercise."""
    t = get(slug)
    return _render_text(
        t, t.script, terms, valuation_date, lead_days, maturity, expiry, start
    )


def render_parts(
    slug: str,
    terms: Mapping[str, float],
    valuation_date: date,
    lead_days: int = LEAD_DAYS,
    maturity: float | None = None,
    expiry: float | None = None,
    start: date | None = None,
) -> List[tuple]:
    """The product's signed parts (Template.parts), each rendered as render
    renders the script: (sign, script). Empty for a product of one script."""
    t = get(slug)
    return [
        (
            sign,
            _render_text(
                t, s, terms, valuation_date, lead_days, maturity, expiry, start
            ),
        )
        for sign, s in t.parts
    ]


def _render_text(t, script, terms, valuation_date, lead_days, maturity, expiry, start):
    slug = t.slug
    names = {p.name for p in t.params}
    unknown = set(terms) - names
    if unknown:
        raise ValueError(
            f"'{slug}' has no term {', '.join(sorted(unknown))} "
            f"(its terms: {', '.join(sorted(names)) or 'none'})"
        )
    if slug in RATES_GENERATED and (maturity is not None or expiry is not None):
        tenor = int(round(maturity)) if maturity is not None else 5
        if not 1 <= tenor <= 30 or (
            maturity is not None and abs(maturity - tenor) > 1e-9
        ):
            raise ValueError("the swap's length is a whole number of years, 1 to 30")
        if expiry is not None and not 0 < expiry <= 30:
            raise ValueError("the expiry is between 0 and 30 years")
        first = min(date.fromisoformat(d) for d in _ISO.findall(t.script))
        script = _rates_body(slug, first, tenor, 1.0 if expiry is None else expiry)
    elif maturity is not None:
        if not 0 < maturity <= 30:
            raise ValueError("the maturity is between 0 and 30 years")
        script = stretch(script, maturity)
    for p in t.params:
        value = float(terms.get(p.name, p.default))
        if not math.isfinite(value):
            raise ValueError(f"term {p.name} must be a finite number")
        script = _assignment(p.name).sub(
            lambda m, v=value: m.group(1) + _fmt(v), script, count=1
        )
    return (
        start_on(script, start)
        if start
        else align_dates(script, valuation_date, lead_days)
    )


def catalog() -> List[dict]:
    """What the site shows of every product (the generated JSON)."""
    return [
        {
            "slug": t.slug,
            "title": t.title,
            "category": t.category,
            "underlyings": t.underlyings,
            "sources": list(t.sources),
            "summary": t.summary,
            "params": [
                {"name": p.name, "default": p.default, "unit": p.unit, "label": p.label}
                for p in t.params
            ],
            "script": t.script,
            "maturity": round(span_years(t.slug), 4),
            **({"quanto": t.quanto} if t.quanto else {}),
            **({"market": t.market} if t.market != "equity" else {}),
            **({"curve": t.curve} if t.curve != "government" else {}),
        }
        for t in templates().values()
    ]


__all__ = [
    "Param",
    "Template",
    "align_dates",
    "catalog",
    "get",
    "parse",
    "render",
    "span_years",
    "start_on",
    "stretch",
]


#: The models under which lastStepDigital's adjoint greeks fail (#372).
#: Heston and SLV: the probability over the last step divides by that step's
#: volatility, sqrt(v), which a calibrated variance that breaks Feller brings
#: near zero, so the greeks have infinite variance (SPY: a delta of 0.39 +-
#: 0.23 then 0.18 +- 0.02, against 0.25 to 0.27 smoothed). Rough Bergomi: no
#: closed form over a step, the slot is the threshold itself and the delta
#: reads zero.
LAST_STEP_FALLBACK_MODELS = ("heston", "slv", "rough_bergomi")

#: The models that cannot condition their step on a knock-out (#369):
#: their step into a date is not lognormal given the path. A library
#: product on knockOutAbove / Below is priced on the thresholds there.
KNOCK_OUT_THRESHOLD_MODELS = ("rough_bergomi", "hull_white")


def smoothed_last_step(script: str, eps: float) -> str:
    """The script with each lastStepDigital(K[, i]) written as the threshold
    it stands for, smoothed over [K - eps, K + eps] like fuzzy's call spread:
    smooth(spot([i]) - (K), eps). Its price moves by O(eps^2); its pathwise
    greeks stay bounded whatever the model's last step. Comments are left
    as they are."""
    lines = []
    for line in script.splitlines(keepends=True):
        code, hash_, comment = line.partition("#")
        lines.append(_smooth_digitals(code, eps) + hash_ + comment)
    return "".join(lines)


def _smooth_digitals(code: str, eps: float) -> str:
    out, pos, name = [], 0, "lastStepDigital("
    while (start := code.find(name, pos)) >= 0:
        i, depth, commas = start + len(name), 1, []
        while depth:
            if i >= len(code):
                raise ValueError("lastStepDigital: unbalanced parentheses")
            c = code[i]
            depth += (c == "(") - (c == ")")
            if c == "," and depth == 1:
                commas.append(i)
            i += 1
        first = start + len(name)
        bounds = [first - 1, *commas, i - 1]
        parts = [code[a + 1 : b].strip() for a, b in zip(bounds, bounds[1:])]
        spot = f"spot({parts[1]})" if len(parts) > 1 else "spot()"
        out.append(code[pos:start] + f"smooth({spot} - ({parts[0]}), {eps:g})")
        pos = i
    return "".join(out) + code[pos:]
