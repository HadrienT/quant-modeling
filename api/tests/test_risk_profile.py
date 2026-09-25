"""The long/short table of the products page (risk_profile.py): the signs a
desk knows for the classic structures come out of the bump-and-reprice, with
paired errors small enough to call them."""

import pytest

from app import risk_profile as rp


@pytest.fixture(autouse=True)
def fast(monkeypatch):
    monkeypatch.setattr(rp, "PATHS_PER_BATCH", 3000)
    rp._profile.cache_clear()
    yield
    rp._profile.cache_clear()


def positions(slug, terms=None):
    _, _, rows = rp.risk_profile(slug, terms)
    return {r.factor.split(" (")[0]: r.position for r in rows}


def test_a_call_is_long_spot_convexity_vol_and_rates_short_dividends():
    p = positions("european-call")
    assert (p["Spot"], p["Spot convexity"], p["Volatility"]) == ("long",) * 3
    assert (p["Interest rates"], p["Dividends"]) == ("long", "short")


def test_an_up_and_out_call_is_short_vega_and_long_skew():
    p = positions("up-and-out-call")
    assert (p["Volatility"], p["Skew"]) == ("short", "long")


def test_a_barrier_reverse_convertible_holder_sold_the_downside():
    p = positions("barrier-reverse-convertible")
    assert (p["Spot"], p["Volatility"], p["Skew"]) == ("long", "short", "short")


def test_a_worst_of_autocall_holder_is_long_correlation():
    p = positions("worst-of-autocall")
    assert (p["Correlation"], p["Volatility"]) == ("long", "short")
    assert "Skew" not in p  # smile factors: one underlying only


def test_every_row_carries_its_paired_error():
    price, se, rows = rp.risk_profile("variance-swap")
    assert price > 0 and se > 0
    for r in rows:
        assert r.std_error >= 0
        if r.position in ("long", "short"):
            assert abs(r.change) > 2 * r.std_error
