"""Term sheet -> payoff script: the structured products of the library as
products with named terms (blueprint/wp/16-scripting.md §8.8).

Each product is one readable script in product_library/*.qms, opening with a
header the site reads:

    # title: Worst-of Phoenix autocall
    # category: Multi-asset
    # underlyings: 3
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
UNITS = ("percent", "amount", "number")

_META = re.compile(r"^# (title|category|underlyings|source|summary): (.+)$")
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


def parse(slug: str, text: str) -> Template:
    meta: Dict[str, str] = {}
    sources: List[str] = []
    params: List[Param] = []
    body: List[str] = []
    for line in text.splitlines():
        m, p = _META.match(line), _PARAM.match(line)
        if p:
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
    )
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


def align_dates(script: str, valuation_date: date) -> str:
    """Moves every date by whole weeks so that the first falls LEAD_DAYS to
    LEAD_DAYS + 6 days after the valuation date."""
    dates = [date.fromisoformat(d) for d in _ISO.findall(script)]
    if not dates:
        return script
    gap = (valuation_date + timedelta(days=LEAD_DAYS) - min(dates)).days
    weeks = -(-gap // 7)  # ceiling
    if weeks == 0:
        return script
    shift = timedelta(weeks=weeks)
    return _ISO.sub(lambda m: (date.fromisoformat(m[0]) + shift).isoformat(), script)


def render(slug: str, terms: Mapping[str, float], valuation_date: date) -> str:
    """The product's script with the given terms (the others at their
    defaults), dated to start a week after `valuation_date`."""
    t = get(slug)
    names = {p.name for p in t.params}
    unknown = set(terms) - names
    if unknown:
        raise ValueError(
            f"'{slug}' has no term {', '.join(sorted(unknown))} "
            f"(its terms: {', '.join(sorted(names)) or 'none'})"
        )
    script = t.script
    for p in t.params:
        value = float(terms.get(p.name, p.default))
        if not math.isfinite(value):
            raise ValueError(f"term {p.name} must be a finite number")
        script = _assignment(p.name).sub(
            lambda m, v=value: m.group(1) + _fmt(v), script, count=1
        )
    return align_dates(script, valuation_date)


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
        }
        for t in templates().values()
    ]


__all__ = ["Param", "Template", "align_dates", "catalog", "get", "parse", "render"]
