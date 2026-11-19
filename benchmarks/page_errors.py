"""Errors of every library product at the Price page's defaults.

Issue #331, step 1 (measurement only). For each product of
api/app/product_library, the request the Price page sends by default
(web/src/shared/products/scripted.ts: model 'auto', SPY -- or AAPL, MSFT,
GOOGL, AMZN for several underlyings, SAP.DE for a quanto --, 4 % rate,
50,000 paths -- 20,000 with several underlyings --, seed 1, AAD greeks,
device 'auto' with Philox), priced in-process through the API's service.

Recorded per product: the relative standard error of the price, of each
greek and of each AAD sensitivity (`risks`), and the discretisation bias. AAD greeks return no bias estimate, so a
second run without greeks (bias_estimate on, the API's default) gives it.

    set -a; . ./.env; set +a
    .venv/bin/python benchmarks/page_errors.py [--only digital] \
        [--out benchmarks/results/<date>-page-errors.json]

Needs the market database (PGHOST) and, to run where the page runs, a wheel
built with CUDA.
"""

from __future__ import annotations

import argparse
import json
import sys
import time
import traceback
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "api"))

from app import pricing_service, product_templates  # noqa: E402
from app.schemas import ScriptedProductRequest  # noqa: E402

MULTI_TICKERS = ["AAPL", "MSFT", "GOOGL", "AMZN"]
GREEKS = ("delta", "gamma", "vega", "theta", "rho", "vanna", "volga")


def page_request(t: product_templates.Template, **override) -> dict:
    """The body the Price page sends for `t` with its default inputs."""
    terms = {p.name: p.default for p in t.params}
    if t.market == "rates":
        body = dict(
            product=t.slug,
            terms=terms,
            hull_white={"curve": "sofr"} if t.curve == "sofr" else {"currency": "USD"},
            greeks_method="aad",
            n_paths=50_000,
            seed=1,
        )
    else:
        multi = t.underlyings > 1
        if multi:
            market = {
                "underlyings": [{"ticker": x} for x in MULTI_TICKERS[: t.underlyings]]
            }
        elif t.quanto:
            market = {"underlyings": [{"ticker": "SAP.DE"}], "currency": t.quanto}
        else:
            market = {"ticker": "SPY"}
        body = dict(
            product=t.slug,
            terms=terms,
            model="auto",
            rate=0.04,
            n_paths=20_000 if multi else 50_000,
            seed=1,
            greeks_method="aad",
            device="auto",
            rng="philox",
            **market,
        )
    body.update(override)
    return body


def _rel(err, value):
    if err is None or value is None or value == 0:
        return None
    return abs(err / value)


def _risks(entries) -> tuple:
    """Scalar sensitivities as they are; a grid's (leverage[i,j], lvol[...],
    thousands of nodes) summed: its parallel value, with no standard error
    (the nodes' errors are correlated)."""
    scalars, grids = [], {}
    for e in entries:
        if "[" in e.label and ("," in e.label or ":" in e.label):
            g = grids.setdefault(e.label.split("[")[0], {"nodes": 0, "sum": 0.0})
            g["nodes"] += 1
            g["sum"] += e.value
        else:
            scalars.append(
                {
                    "label": e.label,
                    "value": e.value,
                    "se": e.std_error,
                    "rel_se": _rel(e.std_error, e.value),
                }
            )
    return scalars, grids


def measure(t: product_templates.Template) -> dict:
    row: dict = {"product": t.slug, "category": t.category}
    req = ScriptedProductRequest.model_validate(page_request(t))
    t0 = time.perf_counter()
    r = pricing_service.price_scripted_product(req)
    row["wall_ms"] = round((time.perf_counter() - t0) * 1e3, 1)
    row["model"] = r.model_choice.model if getattr(r, "model_choice", None) else None
    row["device"], row["gpus"] = r.device, r.gpus
    row["n_paths"] = req.n_paths
    row["npv"], row["se"] = r.npv, r.mc_std_error
    row["rel_se"] = _rel(r.mc_std_error, r.npv)
    row["bias_aad_run"] = r.discretisation_bias
    g = r.greeks.model_dump() if r.greeks is not None else {}
    row["greeks"] = {
        k: {
            "value": g.get(k),
            "se": g.get(f"{k}_std_error"),
            "rel_se": _rel(g.get(f"{k}_std_error"), g.get(k)),
        }
        for k in GREEKS
        if g.get(k) is not None
    }
    # The AAD sensitivities: per-asset deltas and grids live here, not in greeks.
    row["risks"], row["grids"] = _risks(r.risks or [])
    row["bumped"] = list(r.bumped_greeks)

    # The bias: a price-only run, as AAD greeks do not estimate it.
    req_b = ScriptedProductRequest.model_validate(page_request(t, greeks_method="none"))
    t0 = time.perf_counter()
    b = pricing_service.price_scripted_product(req_b)
    row["bias_wall_ms"] = round((time.perf_counter() - t0) * 1e3, 1)
    row["bias"] = b.discretisation_bias
    row["rel_bias"] = _rel(b.discretisation_bias, b.npv)
    row["npv_price_only"], row["se_price_only"] = b.npv, b.mc_std_error
    return row


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default="")
    ap.add_argument("--out", default="benchmarks/results/<date>-page-errors.json")
    args = ap.parse_args()

    rows = []
    for slug, t in sorted(product_templates.templates().items()):
        if args.only and args.only not in slug:
            continue
        try:
            row = measure(t)
        except Exception as exc:  # noqa: BLE001 -- recorded, the table goes on
            row = {"product": slug, "error": f"{type(exc).__name__}: {exc}"}
            traceback.print_exc(file=sys.stderr)
        rows.append(row)
        print(json.dumps(row), flush=True)
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    Path(args.out).write_text(json.dumps(rows, indent=1))


if __name__ == "__main__":
    main()
