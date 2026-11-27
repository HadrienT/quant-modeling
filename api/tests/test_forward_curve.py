"""WP 32 F5: under QM_OPTION_DATA=chain_iv a simulation on one underlying
reads the market's forward curve, every date its own forward and discount,
instead of the flat (rate, yield) WP 31 L6 fitted at the product's last date.

The store is a fake holding a synthetic computed chain whose rate and
dividend yield both rise with the maturity (a flat 20 % smile), so a flat fit
at two years misprices a date at three months by far more than the Monte-Carlo
error. The calibration and the pricing are the real native ones."""

import math
from datetime import date, timedelta

import pytest
import quantmodeling as qm

from app import chain_iv, db, market_snapshot as ms, pricing_service, replay
from app.audit.payloads import MarketInput, MarketInputStatus
from app.schemas import ScriptRequest

SNAP = date(2026, 11, 26)
SPOT, VOL = 100.0, 0.20
TTMS = (0.25, 0.5, 1.0, 1.5, 2.0)


def zero(t):  # 3 % at three months to 5 % at two years
    return 0.03 + 0.02 * (t - 0.25) / 1.75


def yld(t):  # no dividend at three months, 2 % at two years
    return 0.02 * (t - 0.25) / 1.75


def forward(t):
    return SPOT * math.exp((zero(t) - yld(t)) * t)


def discount(t):
    return math.exp(-zero(t) * t)


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


# ── The curve as the market hands it over ─────────────────────────────────


def test_the_curve_is_the_expiries_forwards_on_the_close():
    # An expiry's forward is carried onto the close: F S / S_expiry.
    shifted = [
        db.ChainIvExpiry(
            e.expiry, e.root, e.ttm, e.forward * 1.01, e.discount, SPOT * 1.01, "parity"
        )
        for e in EXPIRIES
    ]
    c = chain_iv.MarketCurve.of(shifted + shifted[:1], SPOT, None)  # a second root
    assert c.times == TTMS
    assert c.forwards == pytest.approx([forward(t) for t in TTMS], rel=1e-14)
    assert c.discounts == pytest.approx([discount(t) for t in TTMS], rel=1e-14)
    # A typed rate replaces the discounts, not the forwards.
    typed = chain_iv.MarketCurve.of(EXPIRIES, SPOT, 0.03)
    assert typed.discounts == () and typed.kwargs()["forward_dfs"] == []


def test_the_market_carries_its_curve_under_chain_iv_only(store, monkeypatch):
    m = ms.local_vol_market("AAA", None, SNAP, 2.0)
    assert m.curve is not None and m.curve_kwargs()["forward_times"] == list(TTMS)
    monkeypatch.setenv(ms.OPTION_DATA_ENV, "legacy")
    assert ms.LocalVolMarket("AAA", SNAP, SPOT, 0.0, [], [], []).curve_kwargs() == {}


def test_one_surface_serves_every_horizon(store):
    # The grid sits on the curve's forwards, not on the flat fit at the
    # horizon: the same grid for a three-month and a two-year product.
    short = ms.local_vol_market("AAA", None, SNAP, 0.25)
    long = ms.local_vol_market("AAA", None, SNAP, 2.0)
    assert (short.rate, short.dividend) != (long.rate, long.dividend)
    assert short.K_grid == long.K_grid
    assert short.sigma_loc_flat == long.sigma_loc_flat
    # Its strikes span the log-moneyness band at the curve's forward at the
    # slices' mid maturity (dupire_from_svi.cpp's F_mid).
    c = qm.ForwardCurve(
        SPOT, list(TTMS), [forward(t) for t in TTMS], [discount(t) for t in TTMS]
    )
    f_mid = c.forward(0.5 * (TTMS[0] + TTMS[-1]))
    assert long.K_grid[0] == pytest.approx(f_mid * math.exp(long.k_min), rel=1e-12)


# ── The acceptance of F5: an early date reads the market's forward ────────


def price(script, model, rate=None, greeks_method="none", n_paths=40_000):
    req = ScriptRequest(
        script=script,
        ticker="AAA",
        model=model,
        rate=rate,
        valuation_date=SNAP,
        n_paths=n_paths,
        seed=7,
        greeks_method=greeks_method,
        # The plain run: its error is the paths', not a multilevel target's.
        mlmc="off",
    )
    return pricing_service.price_script(req)


EARLY, LAST = expiry_of(0.25).isoformat(), expiry_of(2.0).isoformat()
# An observation at three months, the product's last date at two years.
SCRIPT = f"{EARLY}\n    pays spot()\n\n{LAST}\n    pays 1\n"


@pytest.mark.parametrize("model", ["black_scholes", "local_vol", "heston", "slv"])
def test_an_early_date_reads_the_markets_forward(store, model):
    r = price(SCRIPT, model)
    want = discount(0.25) * forward(0.25) + discount(2.0)
    assert abs(r.npv - want) <= 4.0 * r.mc_std_error + 1e-3, (r.npv, want)
    # The flat fit at two years (WP 31 L6) would have missed it.
    flat = SPOT * math.exp(-yld(2.0) * 0.25) + discount(2.0)
    assert abs(flat - want) > 20.0 * r.mc_std_error + 1e-3


def test_a_typed_rate_discounts_the_curves_forwards(store):
    r = price(SCRIPT, "black_scholes", rate=0.03)
    want = math.exp(-0.03 * 0.25) * forward(0.25) + math.exp(-0.03 * 2.0)
    assert abs(r.npv - want) <= 4.0 * r.mc_std_error + 1e-3, (r.npv, want)


def test_the_adjoint_risks_split_rho_by_expiry(store):
    r = price(SCRIPT, "black_scholes", greeks_method="aad", n_paths=8192)
    risks = {x.label: x.value for x in r.risks}
    zeros = {k: v for k, v in risks.items() if k.startswith("zero[")}
    yields = {k: v for k, v in risks.items() if k.startswith("div_yield[")}
    assert len(zeros) == len(TTMS) and len(yields) == len(TTMS)
    assert sum(zeros.values()) == pytest.approx(risks["rate"], rel=1e-9)
    assert sum(yields.values()) == pytest.approx(risks["div"], rel=1e-9)
    # The payment at two years is a zero coupon: -2 DF(2) on its own pillar.
    assert zeros["zero[2]"] == pytest.approx(-2.0 * discount(2.0), rel=1e-6)


# ── F6: the closed forms read the curve at their expiry ───────────────────


def analytic(product, **kw):
    from app.schemas import ScriptedProductRequest

    req = dict(product=product, ticker="AAA", valuation_date=SNAP, maturity=0.75)
    return pricing_service.price_scripted_product(
        ScriptedProductRequest(**{**req, **kw})
    )


@pytest.mark.parametrize(
    "product", ["european-call", "european-put", "digital-call", "equity-forward"]
)
def test_a_closed_form_between_expiries_is_the_monte_carlos(store, product):
    # 0.75 y falls between two expiries: the closed form reads the forward
    # the Monte Carlo steps on (ln F linear in T), not a carry rate
    # interpolated on its own.
    exact = analytic(product, engine="analytic")
    mc = analytic(product, model="black_scholes", n_paths=200_000, seed=3)
    assert exact.mc_std_error == 0.0
    assert abs(exact.npv - mc.npv) <= max(4 * mc.mc_std_error, 1e-9 * abs(exact.npv))
    # Rho and the dividend risk by expiry: exact, summing to the formula's
    # rho, on the two expiries around 0.75 y only.
    risks = {x.label: x.value for x in exact.risks}
    zeros = {k: v for k, v in risks.items() if k.startswith("zero[")}
    assert len(zeros) == len(TTMS) and all(x.std_error == 0.0 for x in exact.risks)
    assert sum(zeros.values()) == pytest.approx(exact.greeks.rho, rel=1e-9)
    read = {k for k, v in risks.items() if v != 0.0}
    assert read == {"zero[0.5]", "zero[1]", "div_yield[0.5]", "div_yield[1]"}


def test_a_typed_rate_keeps_the_curves_forward_in_the_closed_form(store):
    exact = analytic("equity-forward", engine="analytic", rate=0.03)
    mc = analytic("equity-forward", model="black_scholes", rate=0.03, n_paths=50_000)
    assert abs(exact.npv - mc.npv) <= max(4 * mc.mc_std_error, 1e-9 * abs(exact.npv))
    # A flat typed rate has no pillar of its own: the dividend yields only.
    assert not any(x.label.startswith("zero[") for x in exact.risks)


# ── The replay of a valuation recorded before the curve ───────────────────


def test_an_input_read_on_one_side_is_not_a_revision():
    recorded = [{"name": "carry:AAA", "value_hash": "a", "as_of": "2026-11-26"}]

    def read(name, value_hash):
        return MarketInput(
            name=name,
            source="db",
            as_of="2026-11-26",
            status=MarketInputStatus.OBSERVED,
            value_hash=value_hash,
        )

    current = [read("carry:AAA", "a"), read("forward_curve:AAA", "b")]
    assert replay._drifted(recorded, current) == []
    current[0] = read("carry:AAA", "c")
    assert [d.name for d in replay._drifted(recorded, current)] == ["carry:AAA"]
