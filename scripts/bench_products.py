#!/usr/bin/env python3
"""Measure how every library product is priced, and how fast, as the Price
page prices it by default; write web/src/shared/products/methods.gen.json.

For each product of api/app/product_library (the Products page's catalog):

1. a warm-up run (8192 paths), so that the market snapshot, the calibration
   and the compiled GPU kernel are in their caches, as on a running server;
2. the timed run: the page's default market (SPY, or AAPL, MSFT, GOOGL, AMZN
   for several underlyings; USD rates), model 'auto', the price only, to a
   precision target of 0.1 % of the price (the page's default target) under
   a 60 s budget (the schema's maximum). The precision engine
   (api/app/precision.py) runs a pilot, picks the paths and names the rule
   that stopped it; the error it reports includes the time-step bias when
   the model can estimate it. A product the page prices by multilevel QMC
   (local vol, one payoff, no early exercise) is measured with the
   multilevel engine's own target instead, mlmc_rel_eps = 0.1 %: the
   precision mode does not pass its target to that engine yet, which then
   stops at its default 0.2 % (issue noted on the Products page's PR).

What is recorded is what ran: the model the page chose and why, where the
paths ran, the engine's own diagnostics, the steps a year
(pricing_service._steps_per_year, the same function the pricing calls), the
precision outcome and the wall time. The front end turns these facts into
the Products page's "How it is implemented" panel; nothing in that panel is
typed by hand that this file measures.

Run on the server, from the repository root, with the API's environment
(the database of ~/data-ingest; from the host, PGHOST is the container's
address on the dataplatform network):

    PGHOST=172.22.0.5 .venv/bin/python scripts/bench_products.py [slug ...]

Slugs on the command line re-measure those products only and keep the
others' entries. It takes about ten minutes on two V100s.
"""

from __future__ import annotations

import json
import math
import os
import re
import platform
import subprocess
import sys
import time
from datetime import date, datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "web" / "src" / "shared" / "products" / "methods.gen.json"
sys.path.insert(0, str(ROOT / "api"))

#: The page's defaults (web/src/shared/products/scripted.ts).
SINGLE_TICKER = "SPY"
MULTI_TICKERS = ["AAPL", "MSFT", "GOOGL", "AMZN"]
RATE = 0.04
TARGET = 0.001  # 0.1 % of the price
BUDGET_S = 60.0
WARMUP_PATHS = 8192
SWAPTIONS = {"european-swaption", "bermudan-swaption"}
#: The multilevel engine's own line in the diagnostics (mlqmc.hpp describe()).
_MLQMC = "multilevel QMC ("


def _load_env() -> None:
    """The API's .env, without overriding what the shell already set."""
    env = ROOT / ".env"
    if not env.exists():
        return
    for line in env.read_text().splitlines():
        line = line.strip()
        if line and not line.startswith("#") and "=" in line:
            k, v = line.split("=", 1)
            os.environ.setdefault(k.strip(), v.strip().strip("\"'"))


def _request(t, **over):
    """The page's default request body for template t (scriptedRequest)."""
    from app.schemas import ScriptedProductRequest

    body: dict = {"product": t.slug, "terms": {}, "greeks_method": "none"}
    if t.slug in SWAPTIONS:
        body.update(maturity=5, expiry=1)
    if t.market == "rates":
        body["rate"] = RATE
        body["hull_white"] = (
            {"curve": "sofr"} if t.curve == "sofr" else {"currency": "USD"}
        )
    else:
        body.update(model="auto", rate=RATE)
        if t.underlyings > 1:
            body["underlyings"] = [
                {"ticker": x} for x in MULTI_TICKERS[: t.underlyings]
            ]
        else:
            body["ticker"] = SINGLE_TICKER
    body.update(over)
    return ScriptedProductRequest(**body)


def _market_of(t) -> str:
    if t.market == "rates":
        return "USD SOFR swaps" if t.curve == "sofr" else "USD government curve"
    if t.underlyings > 1:
        return ", ".join(MULTI_TICKERS[: t.underlyings])
    return SINGLE_TICKER


def measure(t) -> dict:
    from app import pricing_service as ps
    from app import product_templates

    entry: dict = {
        "slug": t.slug,
        "market": _market_of(t),
        "eps": t.eps,
        "greeks_smoothing": t.greeks if t.eps is not None else None,
        "early_exercise": "exercise(" in t.script,
    }
    if t.quanto:
        entry["skipped"] = (
            "No dividend yield is stored for stocks listed outside the US: "
            "the page cannot price it from the database yet."
        )
        return entry
    try:
        warm = ps.price_scripted_product(_request(t, n_paths=WARMUP_PATHS))
        multilevel = _MLQMC in (warm.diagnostics or "")
        if multilevel:
            # The multilevel engine takes its own target, the same quantity
            # (an RMS error, bias included); the precision mode does not
            # pass its target to it yet (see the module docstring).
            req = _request(t, mlmc_rel_eps=TARGET)
        else:
            req = _request(
                t,
                precision={
                    "mode": "relative",
                    "value": TARGET,
                    "time_budget_s": BUDGET_S,
                },
            )
        start = time.perf_counter()
        r = ps.price_scripted_product(req)
        wall = time.perf_counter() - start
    except Exception as exc:  # recorded, not hidden: the page shows it
        entry["error"] = f"{type(exc).__name__}: {exc}"
        return entry

    choice = r.model_choice
    model = choice.model if choice else ("hull_white" if t.market == "rates" else "")
    script = product_templates.render(t.slug, {}, date.today())
    bias = r.discretisation_bias
    if multilevel:
        diag = r.diagnostics or ""
        points = re.search(r"points per replicate ([\d ]+);", diag)
        reps = re.search(r"(\d+) replicates", diag)
        target = TARGET * abs(r.npv)
        achieved = math.hypot(r.mc_std_error or 0.0, bias or 0.0)
        precision = {
            "target": target,
            "stopped": (
                "multilevel_target" if achieved <= target else "multilevel_short"
            ),
            "paths": (
                int(reps.group(1)) * sum(int(x) for x in points.group(1).split())
                if points and reps
                else None
            ),
            "paths_needed": None,
            "achieved_error": achieved,
            "seconds": round(wall, 3),
            "note": "",
        }
    else:
        p = r.precision
        precision = (
            {
                "target": p.target,
                "stopped": p.stopped,
                "paths": p.paths,
                "paths_needed": p.paths_needed,
                "achieved_error": p.achieved_error,
                "seconds": round(p.seconds, 3) if p.seconds is not None else None,
                "note": p.note,
            }
            if p is not None
            else None
        )
    entry.update(
        model=model,
        model_reason=choice.reason if choice else "",
        device=r.device,
        steps_per_year=(
            None
            # simulated exactly from date to date: no time step
            if model in ("black_scholes", "hull_white")
            else ps._steps_per_year(req, model, script, date.today())
        ),
        variance_scheme=("qe" if model in ("heston", "slv") else None),
        multilevel=multilevel,
        diagnostics=r.diagnostics,
        npv=r.npv,
        std_error=r.mc_std_error,
        bias=bias,
        wall_seconds=round(wall, 3),
        precision=precision,
    )
    return entry


def _machine() -> dict:
    import quantmodeling as qm

    cpu = platform.processor()
    try:
        for line in Path("/proc/cpuinfo").read_text().splitlines():
            if line.startswith("model name"):
                cpu = line.split(":", 1)[1].strip()
                break
    except OSError:
        pass
    gpus = list(qm.gpu_devices()) if qm.gpu_compiled() else []
    commit = subprocess.run(
        ["git", "rev-parse", "--short", "HEAD"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    ).stdout.strip()
    return {
        "cpu": f"{os.cpu_count()} x {cpu}",
        "gpus": gpus,
        "commit": commit,
    }


def main(slugs: list[str]) -> None:
    _load_env()
    from app import product_templates

    previous = {}
    if slugs and OUT.exists():
        previous = {e["slug"]: e for e in json.loads(OUT.read_text())["products"]}
    templates = sorted(product_templates.templates().values(), key=lambda t: t.slug)
    results = dict(previous)
    for t in templates:
        if slugs and t.slug not in slugs:
            continue
        e = measure(t)
        results[t.slug] = e
        p = e.get("precision") or {}
        print(
            f"{t.slug:32s} {e.get('model', '-'):14s} {e.get('device', '-'):4s} "
            f"{p.get('stopped', e.get('error', e.get('skipped', '')))[:40]:40s} "
            f"{e.get('wall_seconds', '')}",
            flush=True,
        )
    OUT.write_text(
        json.dumps(
            {
                "measured_at": datetime.now(timezone.utc).isoformat(timespec="seconds"),
                "machine": _machine(),
                "target": {"mode": "relative", "value": TARGET, "budget_s": BUDGET_S},
                "products": [results[k] for k in sorted(results)],
            },
            indent=1,
        )
        + "\n"
    )
    print(f"wrote {OUT.relative_to(ROOT)}")


if __name__ == "__main__":
    main(sys.argv[1:])
