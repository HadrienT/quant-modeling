"""Pricing slots and cancellation (issue #347): a bounded number of pricings
compute at once, the rest wait for a slot then get a 429; a pricing past its
timeout gets a 408 and stops at its next check, keeping its slot until it
has stopped."""

import asyncio
import math
import threading
import time
from concurrent.futures import ThreadPoolExecutor

import pytest
from fastapi import HTTPException

from app import cancellation
from app import precision as pr
from app.routers import pricing
from app.schemas import PricingResponse, ScriptRequest


@pytest.fixture
def slots(monkeypatch):
    def set_up(n=1, wait=0.2, timeout=0.3):
        monkeypatch.setattr(pricing, "_slots", threading.BoundedSemaphore(n))
        monkeypatch.setattr(pricing, "_SLOT_WAIT_SECONDS", wait)
        monkeypatch.setattr(pricing, "_PRICING_TIMEOUT_SECONDS", timeout)
        return pricing._slots

    return set_up


def _status(coro):
    try:
        asyncio.run(coro)
    except HTTPException as exc:
        return exc.status_code
    return 200


def test_a_pricing_with_every_slot_taken_is_refused_with_429(slots):
    slots(n=1, wait=0.2, timeout=5.0)
    release = threading.Event()

    async def both():
        first = asyncio.ensure_future(pricing._run_with_timeout(release.wait, 5.0))
        await asyncio.sleep(0.05)
        with pytest.raises(HTTPException) as busy:
            await pricing._run_with_timeout(lambda: 1)
        release.set()
        assert await first is True
        return busy.value.status_code

    assert asyncio.run(both()) == 429


def test_a_timed_out_pricing_stops_at_its_next_check_and_keeps_its_slot(slots):
    sem = slots(n=1, wait=0.2, timeout=0.3)
    steps, done = [], threading.Event()

    def long_pricing():
        try:
            for i in range(100):  # 5 s if nothing stopped it
                cancellation.check()
                steps.append(i)
                time.sleep(0.05)
        finally:
            done.set()

    started = time.perf_counter()
    assert _status(pricing._run_with_timeout(long_pricing)) == 408
    assert time.perf_counter() - started < 1.0
    # the zombie still holds the slot until it reaches a check
    assert done.wait(2.0)
    assert len(steps) < 15
    assert sem.acquire(blocking=False)
    sem.release()


def test_the_timeout_counts_from_the_slot_not_from_the_request(slots):
    # one slot, a 0.5 s budget: the second pricing waits 0.3 s for the slot,
    # then computes 0.35 s -- 0.65 s after its request, within its budget
    slots(n=1, wait=2.0, timeout=0.5)

    async def both():
        first = asyncio.ensure_future(pricing._run_with_timeout(time.sleep, 0.3))
        await asyncio.sleep(0.02)
        second = await pricing._run_with_timeout(lambda: (time.sleep(0.35), 2)[1])
        await first
        return second

    assert asyncio.run(both()) == 2


def test_a_parse_takes_no_slot(slots):
    slots(n=1, wait=0.1, timeout=5.0)
    release = threading.Event()

    async def both():
        first = asyncio.ensure_future(pricing._run_with_timeout(release.wait, 5.0))
        await asyncio.sleep(0.05)
        parsed = await pricing._run_with_timeout(lambda: "ok", slot=False)
        release.set()
        await first
        return parsed

    assert asyncio.run(both()) == "ok"


def test_bound_work_on_another_thread_sees_the_flag():
    flag = threading.Event()

    def caller():
        work = cancellation.bind(lambda: "ran")
        flag.set()
        with ThreadPoolExecutor(1) as pool:
            return pool.submit(work).exception()

    assert isinstance(cancellation.run_with(flag, caller), cancellation.Cancelled)


def test_a_precision_target_stops_between_its_runs():
    flag = threading.Event()
    calls = []

    def price(req):
        calls.append(req.n_paths)
        flag.set()  # the route answered 408 during the pilot
        return PricingResponse(
            npv=10.0,
            greeks={},
            diagnostics="d",
            mc_std_error=20.0 / math.sqrt(req.n_paths),
        )

    req = ScriptRequest(
        script="2027-11-19\n    pays 1\n",
        spot=100.0,
        vol=0.2,
        rate=0.03,
        precision={"mode": "absolute", "value": 0.01},
    )
    with pytest.raises(cancellation.Cancelled):
        cancellation.run_with(flag, pr.price_to_precision, req, price)
    assert calls == [pr.PILOT_PATHS]


def test_a_precision_budget_beyond_the_route_timeout_is_capped():
    # an 18 s budget under a route that answers 408 at 2 s: the runs are
    # planned for what the route leaves, and the diagnostics say so
    def price(req):
        return PricingResponse(
            npv=10.0,
            greeks={},
            diagnostics="d",
            mc_std_error=20.0 / math.sqrt(req.n_paths),
        )

    req = ScriptRequest(
        script="2027-11-19\n    pays 1\n",
        spot=100.0,
        vol=0.2,
        rate=0.03,
        precision={"mode": "absolute", "value": 0.01, "time_budget_s": 18.0},
    )
    r = cancellation.run_with(
        threading.Event(),
        pr.price_to_precision,
        req,
        price,
        deadline=time.perf_counter() + 2.0,
    )
    assert "not the 18 s asked" in r.diagnostics
    # outside a route, the budget asked holds
    plain = pr.price_to_precision(req, price)
    assert "asked" not in plain.diagnostics
