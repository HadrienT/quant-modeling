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
    # A few library products: all of them take minutes on a GPU server.
    few = kernel_warmup._library_seeds(qm)[:6]
    monkeypatch.setattr(kernel_warmup, "_library_seeds", lambda q: few)
    done = kernel_warmup.warm_once()
    assert done["failed"] == []
    if not qm.gpu_devices():
        assert done["seeds"] == 0  # nothing to warm on a CPU-only server
        return
    assert done["seeds"] > 40
    assert done["rebuilt"]["directory"] == str(tmp_path)
    # The kernels are on disk: a second pass compiles nothing.
    assert kernel_warmup.warm_once()["compiled"] == 0


def test_every_library_product_is_seeded_with_its_terms_as_data(monkeypatch):
    # Each product of the script library (but the rates products, on the
    # CPU), under each model the GPU runs it with, as the pricing page asks for it; never a CPU-only run (nothing
    # to compile), always with its numbers as terms (one kernel for every
    # term sheet).
    from app import product_templates

    seeds = kernel_warmup._library_seeds(qm)
    labels = [label for label, _ in seeds]
    assert len(set(labels)) == len(labels)
    for slug, t in product_templates.templates().items():
        mine = [label for label in labels if label.split()[1] == slug]
        if t.market == "rates" or "exercise(" in t.script:
            # Hull-White on the curve, and Longstaff-Schwartz, run on the
            # CPU: no kernel to build
            assert not mine, slug
            continue
        assert mine, slug
        if t.eps is not None:
            # the page's adjoint run is smoothed at the product's width
            assert any(f"smoothed at {t.eps:g}" in label for label in mine), slug
        if t.underlyings == 1:
            # Heston and SLV always run on the GPU
            assert any(" heston " in label for label in mine), slug
            assert any(" slv " in label for label in mine), slug
    calls = []
    monkeypatch.setattr(
        qm, "price_script", lambda *a, **kw: calls.append(kw) or {"npv": 0.0}
    )
    for _, price in seeds:
        price()
    assert all(kw["terms_as_parameters"] for kw in calls)
    assert all(kw["device"] != "cpu" for kw in calls)


def test_the_pages_default_runs_come_first():
    # what the Price page asks first -- the adjoint run (smoothed where the
    # product has a width) under the models `auto` picks -- is compiled
    # before any other variant
    labels = [label for label, _ in kernel_warmup._library_seeds(qm)]
    first_other = next(
        i
        for i, label in enumerate(labels)
        if "second order" in label or label.split()[2] not in ("local_vol", "slv")
    )
    head = labels[:first_other]
    assert head and all(" aad" in l or " none" in l for l in head)
    assert "library up-and-out-call slv aad smoothed at 3" in head


def test_one_seed_per_model_prices():
    # The seeds' synthetic inputs are valid requests (on the CPU without a GPU).
    seen = set()
    for label, price in kernel_warmup._library_seeds(qm):
        _, slug, model, *variant = label.split()
        if model in seen or slug != "asian-call":
            continue
        seen.add(model)
        result = price()
        assert result["npv"] == result["npv"], label  # not NaN
    assert seen == {"black_scholes", "local_vol", "heston", "slv"}
