"""The GPU pools given back once the API is idle (issue #350)."""

import pytest

from app import gpu_pool


@pytest.fixture(autouse=True)
def fresh(monkeypatch):
    monkeypatch.setattr(gpu_pool, "_running", 0)
    monkeypatch.setattr(gpu_pool, "_trimmed", True)
    monkeypatch.setattr(gpu_pool, "_last_end", 0.0)
    monkeypatch.setattr(gpu_pool, "IDLE_S", 30.0)


def trims():
    calls = []

    def trim(keep):
        calls.append(keep)
        return 5 << 30

    return calls, trim


def test_nothing_is_trimmed_before_the_first_computation():
    calls, trim = trims()
    assert gpu_pool.trim_if_idle(now=1e6, trim=trim) is None
    assert calls == []


def test_the_pools_are_trimmed_once_after_the_idle_period():
    calls, trim = trims()
    with gpu_pool.busy():
        pass
    end = gpu_pool._last_end
    assert gpu_pool.trim_if_idle(now=end + 10, trim=trim) is None  # still a burst
    assert gpu_pool.trim_if_idle(now=end + 31, trim=trim) == 5 << 30
    assert calls == [gpu_pool.KEEP_BYTES]
    # once per quiet period
    assert gpu_pool.trim_if_idle(now=end + 120, trim=trim) is None
    assert len(calls) == 1


def test_never_while_a_computation_runs():
    calls, trim = trims()
    with gpu_pool.busy():
        assert gpu_pool.trim_if_idle(now=1e9, trim=trim) is None
    assert calls == []


def test_a_new_computation_opens_a_new_quiet_period():
    calls, trim = trims()
    with gpu_pool.busy():
        pass
    gpu_pool.trim_if_idle(now=gpu_pool._last_end + 31, trim=trim)
    with gpu_pool.busy():
        pass
    assert gpu_pool.trim_if_idle(now=gpu_pool._last_end + 31, trim=trim) is not None
    assert len(calls) == 2


def test_the_library_call_answers_without_a_gpu_too():
    import quantmodeling as qm

    assert qm.gpu_trim_idle_pools(gpu_pool.KEEP_BYTES) >= 0
