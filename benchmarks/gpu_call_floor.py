"""Per-call wall time of a GPU pricing, small to large path counts (issue #330).

What a request to the API pays on the card, warm: the library product priced
by pricing_service on typed Black-Scholes inputs (no database), the compiled
kernel already in the cache. The minimum over repeats is the floor; the
median shows the noise. Each price is printed to 17 digits so that a change
meant to keep the bits can be checked against the run before it.

    .venv/bin/python benchmarks/gpu_call_floor.py [--repeats 30] [--device 0]

Run with nothing else on the card (stop the assistant's LLM): it changes
both the free memory the launches are sized from and the timings.
"""

from __future__ import annotations

import argparse
import os
import statistics
import sys
import time
from pathlib import Path

os.environ.setdefault("JWT_SECRET", "benchmark-only-secret-not-for-deployment")
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "api"))

#: (label, library product, greeks): a one-date payoff, whose floor is the
#: cost of the call itself, and a daily path-dependent one at 252 steps a year.
CASES = [
    ("digital-call", "digital-call", "none"),
    ("digital-call aad", "digital-call", "aad"),
    ("phoenix-autocall", "phoenix-autocall", "none"),
    ("phoenix-autocall aad", "phoenix-autocall", "aad"),
]
PATHS = [1024, 16384, 262144, 1048576]


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repeats", type=int, default=30)
    ap.add_argument("--device", default="0", help="CUDA_VISIBLE_DEVICES")
    args = ap.parse_args()
    os.environ["CUDA_VISIBLE_DEVICES"] = args.device

    from app import pricing_service as ps
    from app.schemas import ScriptedProductRequest

    print(f"{'case':22s} {'paths':>8s} {'min ms':>9s} {'median ms':>10s}  npv")
    for label, product, greeks in CASES:
        for n in PATHS:
            req = ScriptedProductRequest(
                product=product,
                spot=100.0,
                vol=0.2,
                dividend=0.0,
                rate=0.04,
                model="black_scholes",
                n_paths=n,
                seed=1,
                greeks_method=greeks,
                device="gpu",
                valuation_date="2026-11-19",
            )
            for _ in range(3):  # compile or read the kernel, warm the card
                r = ps.price_scripted_product(req)
            reps = args.repeats if n <= 262144 else max(3, args.repeats // 6)
            ts = []
            for _ in range(reps):
                t = time.perf_counter()
                r = ps.price_scripted_product(req)
                ts.append(time.perf_counter() - t)
            print(
                f"{label:22s} {n:8d} {min(ts) * 1e3:9.3f} "
                f"{statistics.median(ts) * 1e3:10.3f}  {r.npv:.17g}"
                + (f" delta={r.greeks.delta:.17g}" if greeks == "aad" else "")
            )


if __name__ == "__main__":
    main()
