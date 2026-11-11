"""Time to reach a relative standard error, for every Monte-Carlo route.

blueprint/wp/26-performance.md, lot P0 (issue #206). Runs against the
``quantmodeling`` wheel: what the site runs.

    .venv/bin/python benchmarks/perf_matrix.py [--target 1e-3] [--budget 90]
        [--only barrier] [--no-gpu] [--out benchmarks/results]

For each product, the hand-written engine (the registry's Monte-Carlo route)
and, where a script equivalent is written below, the generic script engine in
four configurations: CPU pseudo-random (PCG32, antithetic), CPU Sobol + bridge
(16 replicates), GPU Philox, GPU Sobol.

Time to error:
- pseudo-random: a pilot run gives the per-path dispersion; the timed run uses
  the number of paths that reaches the target (variance ∝ 1/N);
- Sobol: no such law, so the path count doubles (powers of two) until the
  target is reached; the achieved error is reported, often below the target;
- a run projected (time ∝ paths) beyond the budget is not executed: its
  projected time is reported and flagged.
The timed run is repeated three times, the median is kept.

Every CPU run is single-threaded: no Monte-Carlo pricing of the wheel uses
more than one thread today.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import math
import statistics
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Optional

import quantmodeling as q

VALUATION = dt.date(2026, 1, 1)
S0, K, R, Q, VOL = 100.0, 100.0, 0.03, 0.0, 0.2
WEEKS = [VALUATION + dt.timedelta(days=7 * k) for k in range(1, 53)]
T_WEEKLY = 364.0 / 365.0  # the 52 weekly dates, ACT/365F, as hand-written steps
MAX_PATHS = 1 << 24

# A smooth local-volatility grid: 20 % at the money, a skew, a term structure.
LV_K = [40.0 + 10.0 * i for i in range(17)]  # 40 .. 200
LV_T = [0.1, 0.25, 0.5, 0.75, 1.0, 1.5, 2.0]
LV_SIGMA = [
    max(0.08, 0.2 - 0.12 * math.log(k / S0) + 0.01 * t) for k in LV_K for t in LV_T
]  # row-major K x T


def lv_surface() -> "q.LocalVolSurface":
    s = q.LocalVolSurface()
    s.K_grid, s.T_grid, s.sigma_loc_flat = LV_K, LV_T, LV_SIGMA
    return s


# ── Runs ─────────────────────────────────────────────────────────────────────

# A run prices with n paths and returns (npv, std error, diagnostics).
Run = Callable[[int], tuple]


def hand(fn, make) -> Run:
    def run(n: int):
        res = fn(make(n))
        return res["npv"], res["mc_std_error"], res.get("diagnostics", "")

    return run


def script(text: str, **kwargs) -> Callable[[str, str, str], Run]:
    def config(sampler: str, rng: str, device: str) -> Run:
        def run(n: int):
            res = q.price_script(
                text,
                S0,
                R,
                Q,
                VOL,
                VALUATION.isoformat(),
                n_paths=n,
                seed=1,
                sampler=sampler,
                rng=rng,
                device=device,
                **kwargs,
            )
            return res["npv"], res["mc_std_error"], res.get("diagnostics", "")

        return run

    return config


def dates(ds) -> str:
    return "  ".join(d.isoformat() for d in ds)


def weekly_barrier_script() -> str:
    check = "    if spot() >= 130 then alive = 0 endIf\n"
    return (
        f"{WEEKS[0].isoformat()}\n    alive = 1\n{check}\n"
        f"{dates(WEEKS[1:-1])}\n{check}\n"
        f"{WEEKS[-1].isoformat()}\n{check}"
        "    if alive = 1 then pays max(spot() - 100, 0) endIf\n"
    )


def weekly_lookback_script() -> str:
    return (
        f"{WEEKS[0].isoformat()}\n    m = max(100, spot())\n\n"
        f"{dates(WEEKS[1:-1])}\n    m = max(m, spot())\n\n"
        f"{WEEKS[-1].isoformat()}\n    m = max(m, spot())\n"
        "    pays max(m - 100, 0)\n"
    )


ONE_YEAR = (VALUATION + dt.timedelta(days=365)).isoformat()
DAILY_ASIAN = (
    "2026-01-02\n    acc = spot()\n    n = 1\n\n"
    "schedule(2026-01-05, 2026-12-31, 1D, TARGET, F)\n    acc = acc + spot()\n    n = n + 1\n\n"
    f"{ONE_YEAR}\n    acc = acc + spot()\n    n = n + 1\n"
    "    pays max(acc / n - 100, 0)\n"
)
THREE = dict(
    spots=[100.0, 100.0, 100.0],
    dividends=[0.0, 0.0, 0.0],
    vols=[0.2, 0.25, 0.3],
    correlation=[1, 0.5, 0.5, 0.5, 1, 0.5, 0.5, 0.5, 1],
)
CORR3 = [[1, 0.5, 0.5], [0.5, 1, 0.5], [0.5, 0.5, 1]]
LV_KW = dict(
    model="local_vol",
    K_grid=LV_K,
    T_grid=LV_T,
    sigma_loc_flat=LV_SIGMA,
    steps_per_year=52,
)


# ── Products ─────────────────────────────────────────────────────────────────


def vanilla(n):
    i = q.VanillaBSInput()
    i.spot, i.strike, i.maturity, i.rate, i.dividend, i.vol, i.is_call = (
        S0,
        K,
        1.0,
        R,
        Q,
        VOL,
        True,
    )
    i.n_paths, i.seed = n, 1
    return i


def asian_bs(n):
    i = q.AsianBSInput()
    i.spot, i.strike, i.maturity, i.rate, i.dividend, i.vol, i.is_call = (
        S0,
        K,
        1.0,
        R,
        Q,
        VOL,
        True,
    )
    i.average_type, i.n_paths, i.seed = q.AsianAverageType.Arithmetic, n, 1
    return i


def asian_lv(n):
    i = q.AsianLocalVolInput()
    i.spot, i.strike, i.maturity, i.rate, i.dividend, i.is_call = S0, K, 1.0, R, Q, True
    i.surface, i.average_type, i.n_paths, i.seed = (
        lv_surface(),
        q.AsianAverageType.Arithmetic,
        n,
        1,
    )
    return i


def barrier_bs(greeks: bool):
    def make(n):
        i = q.BarrierBSInput()
        i.spot, i.strike, i.maturity, i.rate, i.dividend, i.vol = (
            S0,
            K,
            T_WEEKLY,
            R,
            Q,
            VOL,
        )
        i.is_call, i.barrier_type, i.barrier_level, i.rebate = (
            True,
            q.BarrierType.UpAndOut,
            130.0,
            0.0,
        )
        i.n_steps, i.brownian_bridge, i.compute_greeks, i.n_paths, i.seed = (
            52,
            False,
            greeks,
            n,
            1,
        )
        return i

    return make


def barrier_lv(n):
    i = q.BarrierLocalVolInput()
    i.spot, i.strike, i.maturity, i.rate, i.dividend, i.surface = (
        S0,
        K,
        T_WEEKLY,
        R,
        Q,
        lv_surface(),
    )
    i.is_call, i.barrier_type, i.barrier_level, i.rebate = (
        True,
        q.BarrierType.UpAndOut,
        130.0,
        0.0,
    )
    i.n_steps, i.brownian_bridge, i.compute_greeks, i.n_paths, i.seed = (
        52,
        False,
        False,
        n,
        1,
    )
    return i


def lookback_bs(n):
    i = q.LookbackBSInput()
    i.spot, i.strike, i.maturity, i.rate, i.dividend, i.vol, i.is_call = (
        S0,
        K,
        T_WEEKLY,
        R,
        Q,
        VOL,
        True,
    )
    i.style, i.extremum = q.LookbackStyle.FixedStrike, q.LookbackExtremum.Maximum
    i.n_steps, i.n_paths, i.seed = 52, n, 1
    return i


def lookback_lv(n):
    i = q.LookbackLocalVolInput()
    i.spot, i.strike, i.maturity, i.rate, i.dividend, i.is_call = (
        S0,
        K,
        T_WEEKLY,
        R,
        Q,
        True,
    )
    i.surface, i.style, i.extremum = (
        lv_surface(),
        q.LookbackStyle.FixedStrike,
        q.LookbackExtremum.Maximum,
    )
    i.n_steps, i.n_paths, i.seed = 52, n, 1
    return i


def basket(n):
    i = q.BasketBSInput()
    i.spots, i.vols, i.dividends, i.weights = (
        THREE["spots"],
        THREE["vols"],
        THREE["dividends"],
        [1 / 3] * 3,
    )
    i.correlations, i.strike, i.maturity, i.rate, i.is_call = CORR3, K, 1.0, R, True
    i.n_paths, i.seed = n, 1
    return i


def rainbow(n):
    i = q.RainbowBSInput()
    i.spots, i.vols, i.dividends, i.correlations = (
        THREE["spots"],
        THREE["vols"],
        THREE["dividends"],
        CORR3,
    )
    i.strike, i.maturity, i.rate, i.is_call, i.notional = 1.0, 1.0, R, True, 100.0
    i.n_paths, i.seed = n, 1
    return i


QUARTERS = [0.25, 0.5, 0.75, 1.0]


def autocall(n):
    i = q.AutocallBSInput()
    i.spot, i.rate, i.dividend, i.vol, i.notional = S0, R, Q, VOL, 100.0
    i.observation_dates, i.autocall_barrier, i.coupon_barrier = QUARTERS, 1.0, 0.8
    i.put_barrier, i.coupon_rate, i.memory_coupon, i.ki_continuous = (
        0.6,
        0.02,
        True,
        False,
    )
    i.n_paths, i.seed = n, 1
    return i


def mountain(n):
    i = q.MountainBSInput()
    i.spots, i.vols, i.dividends, i.correlations = (
        THREE["spots"],
        THREE["vols"],
        THREE["dividends"],
        CORR3,
    )
    i.observation_dates, i.strike, i.rate, i.is_call, i.notional = (
        [1 / 3, 2 / 3, 1.0],  # one date per asset
        0.0,  # strike on the average return
        R,
        True,
        100.0,
    )
    i.n_paths, i.seed = n, 1
    return i


def dispersion(n):
    i = q.DispersionBSInput()
    i.spots, i.vols, i.dividends, i.correlations = (
        THREE["spots"],
        THREE["vols"],
        THREE["dividends"],
        CORR3,
    )
    i.weights, i.maturity, i.rate, i.notional, i.strike_spread = (
        [1 / 3] * 3,
        1.0,
        R,
        100.0,
        0.0,
    )
    i.observation_dates = [k / 52 for k in range(1, 53)]
    i.n_paths, i.seed = n, 1
    return i


def var_swap(n):
    i = q.VarianceSwapBSInput()
    i.spot, i.maturity, i.rate, i.dividend, i.vol, i.notional, i.strike_var = (
        S0,
        1.0,
        R,
        Q,
        VOL,
        100.0,
        0.03,  # off-market: a relative error needs a non-zero value
    )
    i.observation_dates, i.n_paths, i.seed = [k / 52 for k in range(1, 53)], n, 1
    return i


def vol_swap(n):
    i = q.VolatilitySwapBSInput()
    i.spot, i.maturity, i.rate, i.dividend, i.vol, i.notional, i.strike_vol = (
        S0,
        1.0,
        R,
        Q,
        VOL,
        100.0,
        0.15,  # off-market
    )
    i.observation_dates, i.n_paths, i.seed = [k / 52 for k in range(1, 53)], n, 1
    return i


def quanto(n):
    i = q.QuantoBSInput()
    i.spot, i.strike, i.maturity, i.rate_domestic, i.rate_foreign = S0, K, 1.0, R, 0.01
    i.dividend, i.vol, i.fx_rate, i.fx_vol, i.correlation, i.is_call = (
        Q,
        VOL,
        1.1,
        0.1,
        -0.3,
        True,
    )
    i.n_paths, i.seed = n, 1
    return i


@dataclass
class Product:
    key: str
    label: str
    hand: Optional[Run]
    hand_note: str
    script: Optional[Callable[[str, str, str], Run]] = None
    script_note: str = ""
    extra_hand: list = field(default_factory=list)  # (label, run, note)


def products() -> list[Product]:
    return [
        Product(
            "vanilla",
            "Vanilla call (BS)",
            hand(q.price_vanilla_bs_mc, vanilla),
            "with pathwise Greeks",
            script(f"{ONE_YEAR}\n    pays max(spot() - 100, 0)\n"),
        ),
        Product(
            "asian_bs",
            "Arithmetic Asian, 252 fixings (BS)",
            hand(q.price_asian_bs_mc, asian_bs),
            "with Greeks (theta resimulated)",
            script(DAILY_ASIAN),
            "daily TARGET dates (~256), not the engine's 252 equal steps",
        ),
        Product(
            "asian_lv",
            "Arithmetic Asian, 252 fixings (local vol)",
            hand(q.price_asian_lv_mc, asian_lv),
            "with Greeks",
            script(DAILY_ASIAN, **LV_KW),
            "daily TARGET dates (~256)",
        ),
        Product(
            "barrier_bs",
            "Up-and-out call, 52 weekly dates (BS)",
            hand(q.price_barrier_bs_mc, barrier_bs(False)),
            "price only",
            script(weekly_barrier_script()),
            "same 52 dates",
            extra_hand=[
                (
                    "hand-written, with bump Greeks",
                    hand(q.price_barrier_bs_mc, barrier_bs(True)),
                    "9 simulations a path",
                )
            ],
        ),
        Product(
            "barrier_lv",
            "Up-and-out call, 52 weekly dates (local vol)",
            hand(q.price_barrier_lv_mc, barrier_lv),
            "price only",
            script(weekly_barrier_script(), **LV_KW),
            "same 52 dates",
        ),
        Product(
            "lookback_bs",
            "Fixed-strike lookback call, 52 weekly dates (BS)",
            hand(q.price_lookback_bs_mc, lookback_bs),
            "with pathwise Greeks",
            script(weekly_lookback_script()),
            "same 52 dates",
        ),
        Product(
            "lookback_lv",
            "Fixed-strike lookback call, 52 weekly dates (local vol)",
            hand(q.price_lookback_lv_mc, lookback_lv),
            "with Greeks",
            script(weekly_lookback_script(), **LV_KW),
            "same 52 dates",
        ),
        Product(
            "basket",
            "Basket call, 3 assets (BS)",
            hand(q.price_basket_bs_mc, basket),
            "with Greeks",
            script(
                f"{ONE_YEAR}\n    pays max((spot(0) + spot(1) + spot(2)) / 3 - 100, 0)\n",
                **THREE,
            ),
        ),
        Product(
            "worst_of",
            "Worst-of call, 3 assets (BS)",
            hand(q.price_worst_of_bs_mc, rainbow),
            "",
            script(
                f"{ONE_YEAR}\n    pays 100 * max(min(spot(0) / 100, min(spot(1) / 100, spot(2) / 100)) - 1, 0)\n",
                **THREE,
            ),
        ),
        Product(
            "best_of",
            "Best-of call, 3 assets (BS)",
            hand(q.price_best_of_bs_mc, rainbow),
            "",
        ),
        Product(
            "autocall",
            "Autocall, 4 quarterly dates (BS)",
            hand(q.price_autocall_bs_mc, autocall),
            "",
        ),
        Product(
            "mountain",
            "Mountain range, 3 assets, 3 dates (BS)",
            hand(q.price_mountain_bs_mc, mountain),
            "",
        ),
        Product(
            "dispersion",
            "Dispersion swap, 3 assets, 52 dates (BS)",
            hand(q.price_dispersion_bs_mc, dispersion),
            "",
        ),
        Product(
            "var_swap",
            "Variance swap, 52 dates (BS)",
            hand(q.price_variance_swap_bs_mc, var_swap),
            "",
        ),
        Product(
            "vol_swap",
            "Volatility swap, 52 dates (BS)",
            hand(q.price_volatility_swap_bs_mc, vol_swap),
            "",
        ),
        Product("quanto", "Quanto call (BS)", hand(q.price_quanto_bs_mc, quanto), ""),
    ]


SCRIPT_CONFIGS = [
    ("script CPU PCG32", "pseudo", "pcg32", "cpu"),
    ("script CPU Sobol", "sobol", "pcg32", "cpu"),
    ("script GPU Philox", "pseudo", "philox", "gpu"),
    ("script GPU Sobol", "sobol", "pcg32", "gpu"),
]

# ── Time to error ────────────────────────────────────────────────────────────


def timed(run: Run, n: int):
    t0 = time.perf_counter()
    npv, se, diag = run(n)
    return time.perf_counter() - t0, npv, se, diag


def time_to_error(run: Run, target: float, budget: float, quasi: bool) -> dict:
    run(1 << 10)  # warm-up (CUDA context, caches)
    n = 1 << 14
    while True:
        secs, npv, se, diag = timed(run, n)
        rel = se / abs(npv) if npv else math.inf
        if rel <= target:
            break
        if quasi:
            n_next = n * 2
        else:
            n_next = max(n + 1, math.ceil(n * (rel / target) ** 2 * 1.02))
            if n_next % 2:
                n_next += 1  # antithetic pairs
        projected = secs * n_next / n
        if n_next > MAX_PATHS or projected > budget:
            return dict(
                status="projected",
                seconds=projected if not quasi else None,
                paths=n_next,
                npv=npv,
                rel_se=rel,
                at_paths=n,
                at_seconds=secs,
                diagnostics=diag,
            )
        n = n_next
    times = [secs] + [timed(run, n)[0] for _ in range(2)]
    return dict(
        status="measured",
        seconds=statistics.median(times),
        paths=n,
        npv=npv,
        rel_se=rel,
        diagnostics=diag,
    )


def git_sha() -> str:
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "--short", "HEAD"], text=True
        ).strip()
    except Exception:
        return "unknown"


def fmt(r: dict) -> str:
    if r.get("status") == "error":
        return "error"
    s = r["seconds"]
    if r["status"] == "projected":
        return f"~{s:.0f} s (proj.)" if s else f"> budget at {r['at_paths']:,} paths"
    return f"{s:.3f} s" if s < 10 else f"{s:.1f} s"


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--target", type=float, default=1e-3)
    ap.add_argument(
        "--budget", type=float, default=90.0, help="seconds a single run may take"
    )
    ap.add_argument("--only", default="")
    ap.add_argument("--no-gpu", action="store_true")
    ap.add_argument("--out", default="benchmarks/results")
    args = ap.parse_args()

    rows = []
    for p in products():
        if args.only and args.only not in p.key:
            continue
        configs = [("hand-written", p.hand, p.hand_note, False)]
        configs += [(lbl, run, note, False) for lbl, run, note in p.extra_hand]
        if p.script:
            for lbl, sampler, rng, device in SCRIPT_CONFIGS:
                if device == "gpu" and args.no_gpu:
                    continue
                configs.append(
                    (
                        lbl,
                        p.script(sampler, rng, device),
                        p.script_note,
                        sampler == "sobol",
                    )
                )
        for lbl, run, note, quasi in configs:
            try:
                r = time_to_error(run, args.target, args.budget, quasi)
            except Exception as e:  # a route that fails is reported, not hidden
                r = dict(status="error", error=str(e)[:200])
            r.update(product=p.key, label=p.label, config=lbl, note=note)
            rows.append(r)
            print(
                f"{p.key:12s} {lbl:32s} {fmt(r):>18s}  "
                f"npv {r.get('npv', float('nan')):.5g}  rel se {r.get('rel_se', float('nan')):.2e}  "
                f"paths {r.get('paths', 0):,}  {r.get('error', '')}",
                flush=True,
            )

    meta = dict(
        date=dt.datetime.now().isoformat(timespec="seconds"),
        sha=git_sha(),
        target=args.target,
        budget=args.budget,
        python=sys.version.split()[0],
    )
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    path = out / f"{dt.date.today().isoformat()}-{meta['sha']}.json"
    path.write_text(json.dumps(dict(meta=meta, rows=rows), indent=1))
    print(f"\nwritten {path}")

    print(
        f"\n| Product | Configuration | Time to rel. std error {args.target:g} | Paths | Note |"
    )
    print("|---|---|---|---|---|")
    for r in rows:
        print(
            f"| {r['label']} | {r['config']} | {fmt(r)} | {r.get('paths', 0):,} | {r['note']} |"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
