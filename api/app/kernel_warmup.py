"""GPU script kernels made ready before anyone asks (WP 27, lot L1b).

A catalogue product priced on the GPU runs a kernel compiled for its *shape*
(the product's statements, the model, the sampler); its terms and dates are
data. The compiled kernels are kept on disk (QM_KERNEL_CACHE_DIR) and read
back by the next process, so a restart compiles nothing. What is left is the
first use of a shape, and a deployment that changes the kernel every script
links against, which makes every stored kernel stale. A background thread
handles both at API start:

1. the kernels the cache holds from an older build are compiled again from
   the source stored with them (catalogue shapes and users' scripts alike);
2. the catalogue's usual shapes are priced once on a few paths, which
   compiles those the cache has never seen (an empty cache: the first start).

Nothing here is required: a kernel that is not ready is compiled by the
request that needs it (about half a second). Enabled by QM_KERNEL_WARMUP=1
(the compose files set it); a CPU-only server does nothing.
"""

from __future__ import annotations

import logging
import math
import os
import threading
import time
from typing import Callable, List, Optional, Tuple

from .logging_utils import LOGGER_NAME

_log = logging.getLogger(LOGGER_NAME)
_stop = threading.Event()
_thread: Optional[threading.Thread] = None

#: Paths of a seed pricing: enough to launch the kernel, no more.
SEED_PATHS = 4096


def _seeds(q) -> List[Tuple[str, Callable[[], object]]]:
    """The catalogue's usual shapes: (label, a pricing that compiles it).

    The numbers are placeholders (terms and dates are data); what matters is
    the structure each choice gives the script: call or put, the barrier's
    kind, the average's kind, the lookback's style, the number of assets.
    """
    spot, rate, div, vol = 100.0, 0.03, 0.01, 0.2
    strikes = [40.0 + 10.0 * i for i in range(17)]
    tenors = [0.1, 0.25, 0.5, 0.75, 1.0, 1.5, 2.0]

    def surface():
        s = q.LocalVolSurface()
        s.K_grid, s.T_grid = strikes, tenors
        s.sigma_loc_flat = [
            max(0.08, 0.2 - 0.12 * math.log(k / spot) + 0.01 * t)
            for k in strikes
            for t in tenors
        ]
        return s

    def common(i, lv: bool):
        i.spot, i.strike, i.maturity, i.rate, i.dividend = spot, 100.0, 1.0, rate, div
        if lv:
            i.surface = surface()
        else:
            i.vol = vol
        i.n_paths, i.seed = SEED_PATHS, 1
        return i

    three = dict(spots=[100.0] * 3, vols=[0.2, 0.25, 0.3], dividends=[0.0] * 3)
    corr = [[1, 0.5, 0.5], [0.5, 1, 0.5], [0.5, 0.5, 1]]
    weeks = [k / 52 for k in range(1, 53)]
    out: List[Tuple[str, Callable[[], object]]] = []

    def add(label: str, fn, make) -> None:
        out.append((label, lambda: fn(make())))

    for lv in (False, True):
        tag = "lv" if lv else "bs"
        for call in (True, False):
            side = "call" if call else "put"
            if lv:

                def vanilla(call=call):
                    i = q.LocalVolInput()
                    i.spot, i.strike, i.maturity, i.rate, i.dividend = (
                        spot,
                        100.0,
                        1.0,
                        rate,
                        div,
                    )
                    i.is_call, i.K_grid, i.T_grid = call, strikes, tenors
                    i.sigma_loc_flat = surface().sigma_loc_flat
                    i.n_paths, i.seed = SEED_PATHS, 1
                    return i

                add(f"vanilla {side} lv", q.price_local_vol_mc, vanilla)
            for average in (
                q.AsianAverageType.Arithmetic,
                q.AsianAverageType.Geometric,
            ):

                def asian(call=call, average=average, lv=lv):
                    i = common(q.AsianLocalVolInput() if lv else q.AsianBSInput(), lv)
                    i.is_call, i.average_type = call, average
                    return i

                add(
                    f"asian {average.name.lower()} {side} {tag}",
                    q.price_asian_lv_mc if lv else q.price_asian_bs_mc,
                    asian,
                )
            for kind in (
                q.BarrierType.UpAndOut,
                q.BarrierType.UpAndIn,
                q.BarrierType.DownAndOut,
                q.BarrierType.DownAndIn,
            ):

                def barrier(call=call, kind=kind, lv=lv):
                    i = common(
                        q.BarrierLocalVolInput() if lv else q.BarrierBSInput(), lv
                    )
                    up = kind in (q.BarrierType.UpAndOut, q.BarrierType.UpAndIn)
                    i.is_call, i.barrier_type = call, kind
                    i.barrier_level, i.rebate = (130.0 if up else 70.0), 0.0
                    i.n_steps, i.brownian_bridge, i.compute_greeks = 52, False, False
                    return i

                add(
                    f"barrier {kind.name} {side} {tag}",
                    q.price_barrier_lv_mc if lv else q.price_barrier_bs_mc,
                    barrier,
                )
            for style in (q.LookbackStyle.FixedStrike, q.LookbackStyle.FloatingStrike):

                def lookback(call=call, style=style, lv=lv):
                    i = common(
                        q.LookbackLocalVolInput() if lv else q.LookbackBSInput(), lv
                    )
                    i.is_call, i.style = call, style
                    i.extremum = (
                        q.LookbackExtremum.Maximum
                        if call
                        else q.LookbackExtremum.Minimum
                    )
                    i.n_steps = 52
                    return i

                add(
                    f"lookback {style.name} {side} {tag}",
                    q.price_lookback_lv_mc if lv else q.price_lookback_bs_mc,
                    lookback,
                )

    for call in (True, False):
        side = "call" if call else "put"

        def basket(call=call):
            i = q.BasketBSInput()
            i.spots, i.vols, i.dividends = (
                three["spots"],
                three["vols"],
                three["dividends"],
            )
            i.weights, i.correlations = [1 / 3] * 3, corr
            i.strike, i.maturity, i.rate, i.is_call = 100.0, 1.0, rate, call
            i.n_paths, i.seed = SEED_PATHS, 1
            return i

        add(f"basket {side}", q.price_basket_bs_mc, basket)

        def rainbow(call=call):
            i = q.RainbowBSInput()
            i.spots, i.vols, i.dividends = (
                three["spots"],
                three["vols"],
                three["dividends"],
            )
            i.correlations, i.strike, i.maturity = corr, 1.0, 1.0
            i.rate, i.is_call, i.notional = rate, call, 100.0
            i.n_paths, i.seed = SEED_PATHS, 1
            return i

        add(f"worst-of {side}", q.price_worst_of_bs_mc, rainbow)
        add(f"best-of {side}", q.price_best_of_bs_mc, rainbow)

        def mountain(call=call):
            i = q.MountainBSInput()
            i.spots, i.vols, i.dividends = (
                three["spots"],
                three["vols"],
                three["dividends"],
            )
            i.correlations, i.observation_dates = corr, [1 / 3, 2 / 3, 1.0]
            i.strike, i.rate, i.is_call, i.notional = 0.0, rate, call, 100.0
            i.n_paths, i.seed = SEED_PATHS, 1
            return i

        add(f"mountain {side}", q.price_mountain_bs_mc, mountain)

    for memory in (True, False):

        def autocall(memory=memory):
            i = q.AutocallBSInput()
            i.spot, i.rate, i.dividend, i.vol, i.notional = spot, rate, div, vol, 100.0
            i.observation_dates = [0.25, 0.5, 0.75, 1.0]
            i.autocall_barrier, i.coupon_barrier, i.put_barrier = 1.0, 0.8, 0.6
            i.coupon_rate, i.memory_coupon, i.ki_continuous = 0.02, memory, False
            i.n_paths, i.seed = SEED_PATHS, 1
            return i

        add(
            f"autocall {'memory' if memory else 'plain'}",
            q.price_autocall_bs_mc,
            autocall,
        )

    def dispersion():
        i = q.DispersionBSInput()
        i.spots, i.vols, i.dividends = three["spots"], three["vols"], three["dividends"]
        i.correlations, i.weights = corr, [1 / 3] * 3
        i.maturity, i.rate, i.notional, i.strike_spread = 1.0, rate, 100.0, 0.0
        i.observation_dates, i.n_paths, i.seed = weeks, SEED_PATHS, 1
        return i

    add("dispersion swap", q.price_dispersion_bs_mc, dispersion)

    def variance_swap():
        i = q.VarianceSwapBSInput()
        i.spot, i.maturity, i.rate, i.dividend, i.vol = spot, 1.0, rate, div, vol
        i.notional, i.strike_var = 100.0, 0.03
        i.observation_dates, i.n_paths, i.seed = weeks, SEED_PATHS, 1
        return i

    add("variance swap", q.price_variance_swap_bs_mc, variance_swap)

    def volatility_swap():
        i = q.VolatilitySwapBSInput()
        i.spot, i.maturity, i.rate, i.dividend, i.vol = spot, 1.0, rate, div, vol
        i.notional, i.strike_vol = 100.0, 0.15
        i.observation_dates, i.n_paths, i.seed = weeks, SEED_PATHS, 1
        return i

    add("volatility swap", q.price_volatility_swap_bs_mc, volatility_swap)
    return out


def warm_once() -> dict:
    """Rebuild the stale kernels, then price the seeds. Returns what was done:
    {"rebuilt": {...}, "seeds": n, "compiled": n, "failed": [labels], "seconds": s}."""
    import quantmodeling as qm

    started = time.perf_counter()
    done = {"rebuilt": {}, "seeds": 0, "compiled": 0, "failed": [], "seconds": 0.0}
    if not qm.gpu_devices():
        return done
    done["rebuilt"] = dict(qm.rebuild_script_kernels())
    for label, price in _seeds(qm):
        if _stop.is_set():
            break
        try:
            result = price()
        except (
            Exception
        ) as exc:  # noqa: BLE001 -- a seed that fails warms nothing, no more
            done["failed"].append(f"{label}: {exc}")
            continue
        done["seeds"] += 1
        if "compiled in" in str(result.get("diagnostics", "")):
            done["compiled"] += 1
    done["seconds"] = time.perf_counter() - started
    return done


def _run() -> None:
    try:
        done = warm_once()
        _log.info(
            "kernel warm-up: %s stale kernels rebuilt (%s current), %s seeds priced of which %s "
            "compiled, %s failed, %.1f s",
            done["rebuilt"].get("rebuilt", 0),
            done["rebuilt"].get("current", 0),
            done["seeds"],
            done["compiled"],
            len(done["failed"]),
            done["seconds"],
        )
        for failure in done["failed"]:
            _log.warning("kernel warm-up seed failed: %s", failure)
    except Exception:  # noqa: BLE001
        _log.exception("kernel warm-up failed")


def enabled() -> bool:
    return os.getenv("QM_KERNEL_WARMUP", "0").lower() in ("1", "true", "yes")


def start() -> None:
    global _thread
    if _thread is not None or not enabled():
        return
    _stop.clear()
    _thread = threading.Thread(target=_run, name="kernel-warmup", daemon=True)
    _thread.start()


def stop() -> None:
    global _thread
    _stop.set()
    _thread = None
