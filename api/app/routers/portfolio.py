"""Portfolio CRUD, batch pricing, risk & stress-test endpoints."""

import uuid
from datetime import datetime
from typing import Dict, List

from fastapi import APIRouter, Depends, HTTPException
from starlette.concurrency import run_in_threadpool

from ..auth import require_user
from ..logging_utils import get_logger
from ..portfolio_schemas import (
    BatchPriceResponse,
    Portfolio,
    PortfolioRiskSummary,
    PortfolioSummary,
    Position,
    PositionGreeks,
    PositionResult,
    ProductType,
    StressBump,
    StressResult,
    VaRRequest,
    VaRResult,
)
from ..portfolio_storage import (
    delete_portfolio,
    get_portfolio,
    list_portfolios,
    save_portfolio,
)
from ..pricing_service import (
    price_asian,
    price_autocall,
    price_barrier,
    price_basket,
    price_commodity_forward,
    price_commodity_option,
    price_digital,
    price_dispersion_swap,
    price_fixed_rate_bond,
    price_future,
    price_fx_forward,
    price_fx_option,
    price_lookback,
    price_mountain,
    price_rainbow,
    price_vanilla,
    price_variance_swap,
    price_volatility_swap,
    price_zero_coupon_bond,
)
from ..schemas import (
    AsianRequest,
    AutocallRequest,
    BarrierRequest,
    BasketRequest,
    CommodityForwardRequest,
    CommodityOptionRequest,
    DigitalRequest,
    DispersionSwapRequest,
    FixedRateBondRequest,
    FutureRequest,
    FXForwardRequest,
    FXOptionRequest,
    LookbackRequest,
    MountainRequest,
    RainbowRequest,
    VanillaRequest,
    VarianceSwapRequest,
    VolatilitySwapRequest,
    ZeroCouponBondRequest,
)

router = APIRouter(prefix="/api/portfolios", tags=["portfolios"])
logger = get_logger()

# ── Engine label resolution ──────────────────────────────────────────────────

# Default engine per product type (when no explicit engine param)
_DEFAULT_ENGINE: Dict[ProductType, str] = {
    ProductType.european_call:     "BS Analytic",
    ProductType.european_put:      "BS Analytic",
    ProductType.american_call:     "BS Binomial",
    ProductType.american_put:      "BS Binomial",
    ProductType.asian:             "BS Analytic (Geometric)",
    ProductType.barrier:           "BS MC",
    ProductType.digital:           "BS Analytic",
    ProductType.lookback:          "BS MC",
    ProductType.basket:            "BS MC",
    ProductType.zero_coupon_bond:  "Discount Curve",
    ProductType.fixed_rate_bond:   "Discount Curve",
    ProductType.future:            "Cost of Carry",
    ProductType.autocall:          "BS MC",
    ProductType.mountain:          "BS MC",
    ProductType.variance_swap:     "BS MC",
    ProductType.volatility_swap:   "BS MC",
    ProductType.dispersion_swap:   "BS MC",
    ProductType.fx_forward:        "Garman-Kohlhagen",
    ProductType.fx_option:         "Garman-Kohlhagen",
    ProductType.commodity_forward: "Cost of Carry",
    ProductType.commodity_option:  "BS Analytic",
    ProductType.rainbow:           "BS MC",
}

_ENGINE_LABELS = {
    "analytic":   "BS Analytic",
    "mc":         "BS Monte Carlo",
    "binomial":   "BS Binomial",
    "trinomial":  "BS Trinomial",
    "pde":        "BS PDE (Crank-Nicolson)",
}


def _resolve_engine_label(pos: Position, params: dict) -> str:
    """Build a human-readable engine label for a position."""
    explicit = params.get("engine")
    if explicit:
        return _ENGINE_LABELS.get(explicit, explicit)
    return _DEFAULT_ENGINE.get(pos.product_type, "Unknown")


# ── Product-type → (schema, pricer) mapping ─────────────────────────────────

_PRICER_MAP: Dict[ProductType, tuple] = {
    ProductType.european_call:     (VanillaRequest,          price_vanilla),
    ProductType.european_put:      (VanillaRequest,          price_vanilla),
    ProductType.american_call:     (VanillaRequest,          price_vanilla),
    ProductType.american_put:      (VanillaRequest,          price_vanilla),
    ProductType.asian:             (AsianRequest,            price_asian),
    ProductType.barrier:           (BarrierRequest,          price_barrier),
    ProductType.digital:           (DigitalRequest,          price_digital),
    ProductType.lookback:          (LookbackRequest,         price_lookback),
    ProductType.basket:            (BasketRequest,           price_basket),
    ProductType.zero_coupon_bond:  (ZeroCouponBondRequest,   price_zero_coupon_bond),
    ProductType.fixed_rate_bond:   (FixedRateBondRequest,    price_fixed_rate_bond),
    ProductType.future:            (FutureRequest,           price_future),
    ProductType.autocall:          (AutocallRequest,         price_autocall),
    ProductType.mountain:          (MountainRequest,         price_mountain),
    ProductType.variance_swap:     (VarianceSwapRequest,     price_variance_swap),
    ProductType.volatility_swap:   (VolatilitySwapRequest,   price_volatility_swap),
    ProductType.dispersion_swap:   (DispersionSwapRequest,   price_dispersion_swap),
    ProductType.fx_forward:        (FXForwardRequest,        price_fx_forward),
    ProductType.fx_option:         (FXOptionRequest,         price_fx_option),
    ProductType.commodity_forward: (CommodityForwardRequest, price_commodity_forward),
    ProductType.commodity_option:  (CommodityOptionRequest,  price_commodity_option),
    ProductType.rainbow:           (RainbowRequest,          price_rainbow),
}


def _price_position(pos: Position) -> PositionResult:
    """Price a single position using its product type and parameters."""
    entry = _PRICER_MAP.get(pos.product_type)
    if entry is None:
        return PositionResult(
            diagnostics=f"Unsupported product type: {pos.product_type}"
        )
    schema_cls, pricer_fn = entry

    # Inject is_call / is_american for vanilla shortcuts
    params = dict(pos.parameters)
    if pos.product_type in (ProductType.european_call, ProductType.american_call):
        params.setdefault("is_call", True)
    elif pos.product_type in (ProductType.european_put, ProductType.american_put):
        params.setdefault("is_call", False)
    if pos.product_type in (ProductType.american_call, ProductType.american_put):
        params.setdefault("is_american", True)

    # Determine engine label for display
    engine_label = _resolve_engine_label(pos, params)

    try:
        req_obj = schema_cls(**params)
        resp = pricer_fn(req_obj)
        sign = 1.0 if pos.direction == "long" else -1.0
        return PositionResult(
            npv=resp.npv * pos.quantity * sign,
            unit_price=resp.npv,
            greeks=PositionGreeks(
                delta=(resp.greeks.delta or 0.0) * pos.quantity * sign,
                gamma=(resp.greeks.gamma or 0.0) * pos.quantity * sign,
                vega=(resp.greeks.vega or 0.0) * pos.quantity * sign,
                theta=(resp.greeks.theta or 0.0) * pos.quantity * sign,
                rho=(resp.greeks.rho or 0.0) * pos.quantity * sign,
            ),
            priced_at=datetime.utcnow().isoformat(),
            engine=engine_label,
            diagnostics=resp.diagnostics,
            mc_std_error=resp.mc_std_error * pos.quantity,
        )
    except Exception as exc:
        return PositionResult(engine=engine_label, diagnostics=f"Pricing error: {exc}")


def _aggregate_risk(portfolio: Portfolio) -> PortfolioRiskSummary:
    """Aggregate risk from all priced positions."""
    total_npv = 0.0
    total_pnl = 0.0
    total_delta = 0.0
    total_gamma = 0.0
    total_vega = 0.0
    total_theta = 0.0
    total_rho = 0.0
    priced = 0

    for pos in portfolio.positions:
        if pos.result is None:
            continue
        priced += 1
        total_npv += pos.result.npv
        sign = 1.0 if pos.direction == "long" else -1.0
        total_pnl += pos.result.npv - (pos.entry_price * pos.quantity * sign)
        g = pos.result.greeks
        total_delta += g.delta or 0.0
        total_gamma += g.gamma or 0.0
        total_vega += g.vega or 0.0
        total_theta += g.theta or 0.0
        total_rho += g.rho or 0.0

    return PortfolioRiskSummary(
        total_npv=round(total_npv, 6),
        total_pnl=round(total_pnl, 6),
        total_delta=round(total_delta, 6),
        total_gamma=round(total_gamma, 6),
        total_vega=round(total_vega, 6),
        total_theta=round(total_theta, 6),
        total_rho=round(total_rho, 6),
        positions_priced=priced,
        positions_total=len(portfolio.positions),
    )


# ── CRUD ─────────────────────────────────────────────────────────────────────

@router.get("", response_model=List[PortfolioSummary])
async def api_list_portfolios(user: str = Depends(require_user)):
    return await run_in_threadpool(list_portfolios, user)


@router.post("", response_model=Portfolio, status_code=201)
async def api_create_portfolio(name: str = "Untitled Portfolio", user: str = Depends(require_user)):
    pf = Portfolio(id=str(uuid.uuid4()), name=name, owner=user)
    await run_in_threadpool(save_portfolio, pf)
    return pf


@router.get("/{portfolio_id}", response_model=Portfolio)
async def api_get_portfolio(portfolio_id: str, user: str = Depends(require_user)):
    pf = await run_in_threadpool(get_portfolio, portfolio_id, user)
    if pf is None:
        raise HTTPException(status_code=404, detail="Portfolio not found")
    return pf


@router.put("/{portfolio_id}", response_model=Portfolio)
async def api_update_portfolio(portfolio_id: str, body: Portfolio, user: str = Depends(require_user)):
    existing = await run_in_threadpool(get_portfolio, portfolio_id, user)
    if existing is None:
        raise HTTPException(status_code=404, detail="Portfolio not found")
    body.id = portfolio_id
    body.owner = user
    body.updated_at = datetime.utcnow().isoformat()
    await run_in_threadpool(save_portfolio, body)
    return body


@router.delete("/{portfolio_id}", status_code=204)
async def api_delete_portfolio(portfolio_id: str, user: str = Depends(require_user)):
    deleted = await run_in_threadpool(delete_portfolio, portfolio_id, user)
    if not deleted:
        raise HTTPException(status_code=404, detail="Portfolio not found")


# ── Position management ─────────────────────────────────────────────────────

@router.post("/{portfolio_id}/positions", response_model=Portfolio, status_code=201)
async def api_add_position(portfolio_id: str, position: Position, user: str = Depends(require_user)):
    pf = await run_in_threadpool(get_portfolio, portfolio_id, user)
    if pf is None:
        raise HTTPException(status_code=404, detail="Portfolio not found")
    # ensure unique id
    if not position.id:
        position.id = str(uuid.uuid4())
    pf.positions.append(position)
    pf.updated_at = datetime.utcnow().isoformat()
    await run_in_threadpool(save_portfolio, pf)
    return pf


@router.put("/{portfolio_id}/positions/{position_id}", response_model=Portfolio)
async def api_update_position(portfolio_id: str, position_id: str, position: Position, user: str = Depends(require_user)):
    pf = await run_in_threadpool(get_portfolio, portfolio_id, user)
    if pf is None:
        raise HTTPException(status_code=404, detail="Portfolio not found")
    idx = next((i for i, p in enumerate(pf.positions) if p.id == position_id), None)
    if idx is None:
        raise HTTPException(status_code=404, detail="Position not found")
    position.id = position_id
    pf.positions[idx] = position
    pf.updated_at = datetime.utcnow().isoformat()
    await run_in_threadpool(save_portfolio, pf)
    return pf


@router.delete("/{portfolio_id}/positions/{position_id}", response_model=Portfolio)
async def api_remove_position(portfolio_id: str, position_id: str, user: str = Depends(require_user)):
    pf = await run_in_threadpool(get_portfolio, portfolio_id, user)
    if pf is None:
        raise HTTPException(status_code=404, detail="Portfolio not found")
    before = len(pf.positions)
    pf.positions = [p for p in pf.positions if p.id != position_id]
    if len(pf.positions) == before:
        raise HTTPException(status_code=404, detail="Position not found")
    pf.updated_at = datetime.utcnow().isoformat()
    await run_in_threadpool(save_portfolio, pf)
    return pf


# ── Batch pricing ────────────────────────────────────────────────────────────

@router.post("/{portfolio_id}/price", response_model=BatchPriceResponse)
async def api_price_portfolio(portfolio_id: str, user: str = Depends(require_user)):
    pf = await run_in_threadpool(get_portfolio, portfolio_id, user)
    if pf is None:
        raise HTTPException(status_code=404, detail="Portfolio not found")

    # Price every position
    for pos in pf.positions:
        pos.result = await run_in_threadpool(_price_position, pos)

    pf.updated_at = datetime.utcnow().isoformat()
    await run_in_threadpool(save_portfolio, pf)
    risk = _aggregate_risk(pf)
    return BatchPriceResponse(portfolio=pf, risk_summary=risk)


# ── Stress testing ───────────────────────────────────────────────────────────

def _stress_position(pos: Position, bump: StressBump) -> float:
    """Re-price a position with bumped parameters and return PnL vs current."""
    params = dict(pos.parameters)
    # Apply spot bump (relative)
    if "spot" in params and bump.spot_shift != 0.0:
        params["spot"] = params["spot"] * (1.0 + bump.spot_shift)
    # Multi-asset spot bumps
    if "spots" in params and bump.spot_shift != 0.0:
        params["spots"] = [s * (1.0 + bump.spot_shift) for s in params["spots"]]
    # Vol bump (absolute)
    if "vol" in params and bump.vol_shift != 0.0:
        params["vol"] = max(0.001, params["vol"] + bump.vol_shift)
    if "vols" in params and bump.vol_shift != 0.0:
        params["vols"] = [max(0.001, v + bump.vol_shift) for v in params["vols"]]
    # Rate bump (absolute)
    if "rate" in params and bump.rate_shift != 0.0:
        params["rate"] = params["rate"] + bump.rate_shift
    if "rate_domestic" in params and bump.rate_shift != 0.0:
        params["rate_domestic"] = params["rate_domestic"] + bump.rate_shift

    bumped_pos = pos.model_copy(update={"parameters": params})
    bumped_result = _price_position(bumped_pos)
    base_npv = pos.result.npv if pos.result else 0.0
    return bumped_result.npv - base_npv


@router.post("/{portfolio_id}/stress", response_model=List[StressResult])
async def api_stress_test(portfolio_id: str, bumps: List[StressBump], user: str = Depends(require_user)):
    pf = await run_in_threadpool(get_portfolio, portfolio_id, user)
    if pf is None:
        raise HTTPException(status_code=404, detail="Portfolio not found")

    results: List[StressResult] = []
    for bump in bumps:
        position_pnls: Dict[str, float] = {}
        portfolio_pnl = 0.0
        for pos in pf.positions:
            pnl = await run_in_threadpool(_stress_position, pos, bump)
            position_pnls[pos.id] = round(pnl, 6)
            portfolio_pnl += pnl
        results.append(StressResult(
            name=bump.name,
            portfolio_pnl=round(portfolio_pnl, 6),
            position_pnls=position_pnls,
        ))
    return results


# ── VaR (delta-gamma with Cornish-Fisher) ────────────────────────────────────


def _extract_spot_vol(pos: Position) -> tuple:
    """Return (spot, vol) for a position from its parameters."""
    s = pos.parameters.get("spot")
    v = pos.parameters.get("vol")
    if s is None and "spots" in pos.parameters:
        spots = pos.parameters["spots"]
        s = sum(spots) / max(1, len(spots)) if spots else None
    if v is None and "vols" in pos.parameters:
        vols = pos.parameters["vols"]
        v = sum(vols) / max(1, len(vols)) if vols else None
    return (float(s) if s else 0.0, float(v) if v else 0.0)


@router.post("/{portfolio_id}/var", response_model=VaRResult)
async def api_compute_var(portfolio_id: str, req: VaRRequest, user: str = Depends(require_user)):
    """Delta-gamma VaR with Cornish-Fisher skewness adjustment.

    For each position i with underlying spot S_i, implied vol σ_i:
      daily return std  = σ_i / √252
      horizon std       = daily_σ × √h
      dollar move std   = S_i × horizon_σ

    Position P&L (second-order Taylor):
      ΔP_i ≈ δ_i·ΔS + ½·Γ_i·(ΔS)²

    Portfolio moments of the P&L under Gaussian ΔS:
      μ₁ = Σ ½·Γ_i·σ²_$  (mean from gamma)
      μ₂ = Σ δ_i²·σ²_$ + ½·Γ_i²·σ⁴_$·2  (variance)
      μ₃ = Σ 3·δ_i·Γ_i²·σ⁴_$·... → skewness from gamma

    Then Cornish-Fisher expansion adjusts the z-quantile for
    the non-zero skewness induced by gamma.
    """
    import math
    from scipy.stats import norm as sp_norm  # type: ignore

    pf = await run_in_threadpool(get_portfolio, portfolio_id, user)
    if pf is None:
        raise HTTPException(status_code=404, detail="Portfolio not found")

    z_alpha = sp_norm.ppf(req.confidence)

    # Accumulate per-position contributions to portfolio P&L moments
    # assuming independent underlyings (conservative for diversified books)
    mu1 = 0.0     # E[ΔP]
    var_pnl = 0.0 # Var[ΔP]
    m3 = 0.0      # third central moment contribution

    for pos in pf.positions:
        if pos.result is None:
            continue
        g = pos.result.greeks
        delta_i = g.delta or 0.0
        gamma_i = g.gamma or 0.0

        spot_i, vol_i = _extract_spot_vol(pos)
        if spot_i < 1e-12 or vol_i < 1e-12:
            continue

        # Dollar move standard deviation over the horizon
        daily_sigma = vol_i / math.sqrt(252)
        horizon_sigma = daily_sigma * math.sqrt(req.horizon_days)
        sigma_dollar = spot_i * horizon_sigma          # std(ΔS)
        sigma2 = sigma_dollar ** 2                     # Var(ΔS)
        sigma4 = sigma2 ** 2                           # (Var(ΔS))²

        # E[ΔP_i] = ½·Γ·Var(ΔS)
        mu1 += 0.5 * gamma_i * sigma2

        # Var[ΔP_i] = δ²·σ²_$ + ½·Γ²·2·σ⁴_$  (from E[(ΔS)⁴]=3σ⁴)
        var_pnl += delta_i ** 2 * sigma2 + 0.5 * gamma_i ** 2 * sigma4

        # Third moment: E[(ΔP_i - E[ΔP_i])³]
        # dominated by 3·δ²·Γ·σ⁴  (cross term)
        m3 += 3.0 * delta_i ** 2 * gamma_i * sigma4

    sigma_pnl = math.sqrt(var_pnl) if var_pnl > 0 else 1e-12

    # Skewness
    skew = m3 / (sigma_pnl ** 3) if sigma_pnl > 1e-12 else 0.0

    # Cornish-Fisher adjusted quantile:
    #   z_cf = z + (z² - 1)·S/6
    z_cf = z_alpha + (z_alpha ** 2 - 1) * skew / 6.0

    # VaR = -(μ₁ + z_cf · σ_P)  (loss, so negate)
    var_value = -(mu1 + z_cf * sigma_pnl)

    # Expected Shortfall (Gaussian base, adjusted)
    es_gaussian = sp_norm.pdf(z_alpha) / (1 - req.confidence) * sigma_pnl
    # Add gamma mean shift
    es_value = es_gaussian - mu1

    return VaRResult(
        confidence=req.confidence,
        horizon_days=req.horizon_days,
        var=round(max(var_value, 0.0), 2),
        expected_shortfall=round(max(es_value, 0.0), 2),
        method="delta-gamma (Cornish-Fisher)",
    )
