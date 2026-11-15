"""The GPU kernel warm-up (WP 27, lot L1b): its seeds are valid requests, and
it does nothing it should not."""

import quantmodeling as qm

from app import kernel_warmup


def test_every_seed_is_a_valid_request():
    # On the CPU or the GPU, whichever the server has: a seed whose inputs
    # the library refuses would warm nothing, silently.
    seeds = kernel_warmup._seeds(qm)
    assert len(seeds) > 40
    assert len({label for label, _ in seeds}) == len(seeds)
    for label, price in seeds:
        result = price()
        assert result["npv"] == result["npv"], label  # not NaN


def test_off_unless_asked(monkeypatch):
    monkeypatch.delenv("QM_KERNEL_WARMUP", raising=False)
    assert not kernel_warmup.enabled()
    monkeypatch.setenv("QM_KERNEL_WARMUP", "1")
    assert kernel_warmup.enabled()


def test_warm_once_reports(monkeypatch, tmp_path):
    monkeypatch.setenv("QM_KERNEL_CACHE_DIR", str(tmp_path))
    done = kernel_warmup.warm_once()
    assert done["failed"] == []
    if not qm.gpu_devices():
        assert done["seeds"] == 0  # nothing to warm on a CPU-only server
        return
    assert done["seeds"] > 40
    assert done["rebuilt"]["directory"] == str(tmp_path)
    # The kernels are on disk: a second pass compiles nothing.
    assert kernel_warmup.warm_once()["compiled"] == 0
