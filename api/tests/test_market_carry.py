"""WP 31 L6 (decision D1): under QM_OPTION_DATA=chain_iv a product is priced
on the market's forward and discount at its horizon, so a vanilla expiring
then reprices on the surface's forward under every model, and a typed rate
replaces the discount only.

The store is a fake holding a synthetic computed chain: a flat 20 % smile on
forwards F(T) = 100 exp(0.02 T) and discounts DF(T) = exp(-0.045 T), far
from any flat (rate, yield) a request would type. The calibration and the
pricing are the real native ones."""

import math
from datetime import date, timedelta

import pytest

from app import chain_iv, db, market_snapshot as ms, pricing_service
from app.schemas import ScriptRequest

SNAP = date(2026, 11, 26)
SPOT, CARRY, RATE, VOL = 100.0, 0.02, 0.045, 0.20
TTMS = (0.25, 0.5, 1.0, 1.5, 2.0)


def forward(t):
    return SPOT * math.exp(CARRY * t)


def discount(t):
    return math.exp(-RATE * t)


def black(f, k, t, vol, df, call=True):
    s = vol * math.sqrt(t)
    d1 = (math.log(f / k) + 0.5 * s * s) / s
    n = lambda x: 0.5 * math.erfc(-x / math.sqrt(2.0))  # noqa: E731
    c = df * (f * n(d1) - k * n(d1 - s))
    return c if call else c - df * (f - k)


def expiry_of(t):
    return SNAP + timedelta(days=round(t * 365))


def rows():
    out = []
    for t in TTMS:
        f, df = forward(t), discount(t)
        for i in range(-16, 17):
            k = 0.025 * i
            strike = f * math.exp(k)
            call = strike >= f
            price = black(f, strike, t, VOL, df, call)
            out.append(
                db.ChainIvRow(
                    expiry=expiry_of(t),
                    root="AAA",
                    option_type="call" if call else "put",
                    strike=strike,
                    ttm=t,
                    bid=price * 0.99,
                    ask=price * 1.01,
                    mid=price,
                    exercise="european",
                    forward=f,
                    discount=df,
                    implied_vol=VOL,
                    log_moneyness=k,
                    flags=0,
                )
            )
    return out


EXPIRIES = [
    db.ChainIvExpiry(expiry_of(t), "AAA", t, forward(t), discount(t), SPOT, "parity")
    for t in TTMS
]


@pytest.fixture
def store(monkeypatch):
    ms.clear_caches()
    monkeypatch.setenv(ms.OPTION_DATA_ENV, "chain_iv")
    chain = rows()
    monkeypatch.setattr(
        db,
        "options_chain_iv_load_on_or_before",
        lambda t, d: db.ChainIvLoad(SNAP, len(chain), "synthetic", 1, "parity"),
    )
    monkeypatch.setattr(db, "options_chain_iv", lambda t, d: chain)
    monkeypatch.setattr(db, "options_chain_iv_expiries", lambda t, d: EXPIRIES)
    monkeypatch.setattr(db, "price_on_or_before", lambda t, d: (SNAP, SPOT))
    yield
    ms.clear_caches()


# ── The carry itself ──────────────────────────────────────────────────────


def test_the_carry_is_the_markets_discount_and_forward_at_the_horizon():
    c = chain_iv.MarketCarry.at(EXPIRIES, SPOT, 1.0, None)
    assert c.market_rate == pytest.approx(RATE, abs=1e-14)
    assert c.carry == pytest.approx(CARRY, abs=1e-14)
    assert (c.rate, c.dividend) == pytest.approx((RATE, RATE - CARRY), abs=1e-14)
    # Between two expiries, the carry and the zero rate are interpolated.
    mid = chain_iv.MarketCarry.at(EXPIRIES, SPOT, 0.75, None)
    assert mid.carry == pytest.approx(CARRY, abs=1e-14)


def test_a_typed_rate_replaces_the_discount_and_keeps_the_forward():
    c = chain_iv.MarketCarry.at(EXPIRIES, SPOT, 1.0, 0.03)
    assert c.typed and c.rate == 0.03
    assert c.rate - c.dividend == pytest.approx(CARRY, abs=1e-14)


def test_legacy_still_requires_a_rate(monkeypatch):
    monkeypatch.setenv(ms.OPTION_DATA_ENV, "legacy")
    with pytest.raises(ValueError, match="rate is required"):
        ms.local_vol_market("AAA", None, SNAP)


def test_the_market_is_fitted_to_the_horizon_it_is_asked_for(store):
    m = ms.local_vol_market("AAA", None, SNAP, 1.5)
    assert m.carry.horizon == 1.5
    assert m.rate - m.dividend == pytest.approx(CARRY, abs=1e-12)
    codes = [w["code"] for w in m.warnings]
    assert "market_carry" in codes and "rate_override" not in codes
    typed = ms.local_vol_market("AAA", 0.03, SNAP, 1.5)
    assert "rate_override" in [w["code"] for w in typed.warnings]


# ── The acceptance of L6: repricing on the surface's forward ───────────────


def price(script, model, rate=None, n_paths=40_000):
    req = ScriptRequest(
        script=script,
        ticker="AAA",
        model=model,
        rate=rate,
        valuation_date=SNAP,
        n_paths=n_paths,
        seed=7,
    )
    return pricing_service.price_script(req)


ONE_YEAR = expiry_of(1.0).isoformat()


@pytest.mark.parametrize("model", ["black_scholes", "local_vol"])
def test_a_forward_reprices_on_the_markets_forward_under_every_model(store, model):
    strike = 100.0
    r = price(f"{ONE_YEAR}\n    pays spot() - {strike}\n", model)
    want = discount(1.0) * (forward(1.0) - strike)
    assert abs(r.npv - want) <= 4.0 * r.mc_std_error + 1e-3, (r.npv, want)


def test_a_typed_rate_discounts_the_markets_forward(store):
    strike = 100.0
    r = price(f"{ONE_YEAR}\n    pays spot() - {strike}\n", "black_scholes", rate=0.03)
    want = math.exp(-0.03) * (forward(1.0) - strike)
    assert abs(r.npv - want) <= 4.0 * r.mc_std_error + 1e-3, (r.npv, want)


def test_a_call_reprices_on_the_surface_under_local_vol(store):
    f = forward(1.0)
    for strike in (0.9 * f, f, 1.1 * f):
        r = price(f"{ONE_YEAR}\n    pays max(spot() - {strike}, 0)\n", "local_vol")
        want = black(f, strike, 1.0, VOL, discount(1.0))
        # The MC error and the Euler step's bias on a flat 20 % local vol.
        assert abs(r.npv - want) <= 4.0 * r.mc_std_error + 0.02, (strike, r.npv, want)
