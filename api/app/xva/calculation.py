"""xVA of a netting set and its sensitivities: one call to the library each."""

from __future__ import annotations

import time
from datetime import date
from typing import TYPE_CHECKING, Callable, List, Optional, Tuple, Union

import quantmodeling as qm

from .. import xva_fx

if TYPE_CHECKING:  # pragma: no cover — xva_equity imports the portfolio modules
    from ..xva_equity import AssetMarket
from ..xva_schemas import (
    AdjustmentRisks,
    CorrelationEstimate,
    EquityMarket,
    ExposureProfileOut,
    ForeignMarket,
    FxVolatilityPoint,
    HistoricalDynamics,
    HullWhiteInput,
    InitialMarginOut,
    SaCvaOut,
    SensitivityAdjustments,
    XvaMarket,
    XvaRequest,
    XvaResponse,
    XvaSensitivitiesRequest,
    XvaSensitivitiesResponse,
    XvaTrade,
)
from .base import NOTIONAL, XvaInputError, XvaUnavailable, _BUSINESS_DAYS, _FX_KINDS
from .demos import PORTFOLIOS, _Spec
from .market import _Rates, _credit, _historical, _rates_market
from .trades import (
    _adjustments,
    _asset_trades,
    _has_asset,
    _pricing_exposure,
    _profile,
    _script_specs,
    _trades,
    _two_currencies,
    _two_currency_trades,
)
from .capital import (
    _INVESTMENT_GRADE,
    _capital,
    _capital_inputs,
    _capital_method,
    _default_rate,
    _margin_model,
)
from .methodology_text import methodology, sensitivities_methodology

#: The trades of a netting set that is not one of PORTFOLIOS, once the date
#: of the market is known: (label, what to say about it, trades, warnings,
#: the share its equity options are on — None without any).
Book = Callable[
    [date], Tuple[str, str, List[_Spec], List[str], Optional["AssetMarket"]]
]


def _netting_set(
    req: Union[XvaRequest, XvaSensitivitiesRequest],
    book: Optional[Book],
    rates: _Rates,
    warnings: List[str],
) -> Tuple[str, str, List[_Spec], Optional["AssetMarket"]]:
    """Label, lesson, trades and share market of the netting set: a
    portfolio's (`book`, whose notes join `warnings`) or a demonstration one."""
    if book is not None:
        label, lesson, specs, said, asset = book(rates.as_of)
        warnings.extend(said)
        return label, lesson, specs, asset
    if req.portfolio not in PORTFOLIOS:
        raise XvaInputError(
            "A netting set of a portfolio is computed from the Portfolio page, "
            "which sends its trades."
        )
    label, lesson, specs = PORTFOLIOS[req.portfolio]
    return label, lesson, specs, None


def _two_currency_market(rates: _Rates) -> xva_fx.TwoCurrencyMarket:
    try:
        return xva_fx.market(
            rates.as_of, rates.times, rates.dfs, (rates.mean_reversion, rates.sigma)
        )
    except xva_fx.TwoCurrencyUnavailable as exc:
        raise XvaUnavailable(str(exc)) from exc


def _csa_argument(req: Union[XvaRequest, XvaSensitivitiesRequest]) -> Optional[dict]:
    """The CSA as the library reads it: margin period of risk in years."""
    if req.csa is None:
        return None
    return {
        "threshold_counterparty": req.csa.threshold_counterparty,
        "threshold_bank": req.csa.threshold_bank,
        "minimum_transfer_amount": req.csa.minimum_transfer_amount,
        "margin_period_of_risk": req.csa.margin_period_of_risk_days / _BUSINESS_DAYS,
        "cashflows": req.csa.cashflows,
    }


def compute(
    req: XvaRequest, today: Optional[date] = None, book: Optional[Book] = None
) -> XvaResponse:
    started = time.perf_counter()
    warnings: List[str] = []
    rates = _rates_market(today)
    label, lesson, specs, asset = _netting_set(req, book, rates, warnings)
    if req.scripts and _two_currencies(specs):
        raise XvaInputError(
            "A scripted trade is valued by regression, which the two-currency "
            "simulation does not do: add scripts to a portfolio in dollars."
        )
    specs = specs + _script_specs(req, rates)
    if not specs:
        raise XvaInputError(
            "This netting set holds no trade the exposure engine can simulate."
            if book is not None
            else "The portfolio `custom` is made of the scripts of the request: give "
            "at least one."
        )
    counterparty = _credit(req.counterparty_rating, req.recovery, rates, warnings)
    own = _credit(req.own_rating, req.recovery, rates, warnings)
    foreign: Optional[xva_fx.TwoCurrencyMarket] = None
    historical: Optional[HistoricalDynamics] = None
    asset_argument: Optional[dict] = None
    asset_volatility = 0.0
    if _has_asset(specs):
        if asset is None:
            raise XvaInputError("An equity option needs the market of its share.")
        if req.device == "gpu":
            raise XvaInputError(
                "The simulation of a share with the interest rate runs on the CPU: "
                "ask for `auto` or `cpu`."
            )
        trades, asset_argument, asset_volatility = _asset_trades(specs, rates, asset)
    elif _two_currencies(specs):
        if req.device == "gpu":
            raise XvaInputError(
                "The two-currency simulation runs on the CPU: ask for `auto` or `cpu`."
            )
        foreign = _two_currency_market(rates)
        trades = _two_currency_trades(specs, rates, foreign)
    else:
        historical = _historical(req)
        trades = _trades(specs, rates)

    csa = _csa_argument(req)
    lgd = 1.0 - req.recovery
    horizon = [max(s.start + s.tenor for s in specs)]
    margin = None
    margin_model, margin_reason = "regression", ""
    if req.csa is not None and req.csa.initial_margin:
        margin_model, margin_reason = _margin_model(req, specs)
        margin = {
            "confidence": 0.99,
            "im_today": req.csa.initial_margin_today,
            # Segregated margin is funded at the bank's borrowing spread.
            "spread": req.borrowing_spread,
            "model": margin_model,
        }
    default_rate = (
        _default_rate(req.counterparty_rating) if req.capital.pd is None else None
    )
    capital_method, capital_reason = _capital_method(req, specs)
    capital = _capital_inputs(req, counterparty, default_rate, capital_method)
    result = qm.xva_netting_set(
        rates.times,
        rates.dfs,
        (rates.mean_reversion, rates.sigma),
        trades,
        (horizon, [counterparty.hazard]),
        (horizon, [own.hazard]),
        lgd,
        lgd,
        csa,
        req.borrowing_spread,
        req.lending_spread,
        (
            (historical.mean_reversion, historical.long_run_rate, historical.sigma)
            if historical is not None
            else None
        ),
        req.paths,
        req.seed,
        req.pfe_confidence,
        initial_margin=margin,
        collateral_spread=req.csa.collateral_rate_spread if req.csa else 0.0,
        capital=capital,
        # Per unit of currency in the library; per reference notional here.
        wrong_way_b=req.wrong_way_risk / NOTIONAL,
        device=req.device,
        foreign=(foreign.argument() if foreign is not None else asset_argument),
    )
    if asset is not None and asset_argument is not None:
        warnings.append(
            f"The correlation of {asset.ticker} with the interest rate is a "
            "historical estimate on weekly changes: no freely published price "
            "implies it."
        )
        if asset.volatility_proxied:
            warnings.append(
                f"No option chain is stored for {asset.ticker}: its volatility is "
                "its realised volatility over 63 business days, a proxy."
            )
        if asset.dividend_as_of is None:
            warnings.append(
                f"No dividend yield is stored for {asset.ticker}: it is taken as zero."
            )
        if sum(1 for s in specs if s.kind == "equity_option") > 1:
            warnings.append(
                f"The share has one volatility in the model, calibrated on the option "
                f"struck at {asset.calibration_strike:g} expiring in "
                f"{asset.calibration_expiry:.2f} years: the other options are valued "
                "at it, without the smile."
            )

    if foreign is not None:
        warnings.append(
            "The three correlations — between dollar and euro rates, and of each "
            "with the exchange rate — are historical estimates on weekly changes: "
            "no freely published price implies them."
        )
        longest = max(s.start + s.tenor for s in specs)
        if longest > foreign.calibration_expiry + 1e-9:
            warnings.append(
                f"The volatility of the exchange rate is calibrated on options of "
                f"{foreign.calibration_expiry:g} year(s), the longest expiry traded "
                f"in number; these trades run to {longest:g} years, where it is an "
                "extrapolation."
            )
    warnings.append(
        "Counterparty and own credit curves are rating proxies built from bond "
        "spreads (ICE BofA indices), not CDS quotes of a name."
    )
    if (
        historical is not None
        and historical.estimated_mean_reversion is not None
        and not {
            "mean_reversion",
            "long_run_rate",
        }
        <= set(historical.overridden)
    ):
        warnings.append(
            "The long-run rate and the mean reversion of the historical measure are "
            "estimates that change with the window; the risk measures depend on them."
        )
    rows = []
    for spec, trade, value, c in zip(
        specs, trades, result["trade_values_today"], result["contributions"]
    ):
        rows.append(
            XvaTrade(
                description=spec.description,
                kind=spec.kind,
                currency="EUR" if spec.kind in _FX_KINDS else spec.currency,
                # (an equity option: `payer` says call or put)
                quantity=spec.quantity,
                # A script of the request has only its text; the swap this
                # module writes as a script keeps the terms it was written from.
                payer=None if spec.script is not None else spec.payer,
                notional=None if spec.script is not None else spec.notional,
                fixed_rate=(
                    None
                    if spec.kind == "cashflow"
                    else trade.get(
                        "fixed_rate", trade.get("strike", trade.get("domestic_rate"))
                    )
                ),
                foreign_rate=trade.get("foreign_rate"),
                start=None if spec.script is not None else spec.start,
                tenor=None if spec.script is not None else spec.tenor,
                script=trade.get("script"),
                maturity=spec.start + spec.tenor,
                value_today=value,
                standalone_cva=c["standalone_cva"],
                incremental_cva=c["incremental_cva"],
                marginal_cva=c["marginal_cva"],
            )
        )
    open_set = result["uncollateralised"]
    return XvaResponse(
        portfolio=req.portfolio,
        portfolio_label=label,
        lesson=lesson,
        value_today=sum(result["trade_values_today"]),
        trades=rows,
        exposure=_pricing_exposure(result["exposure"]),
        exposure_uncollateralised=(
            _pricing_exposure(open_set["exposure"]) if open_set else None
        ),
        risk=(
            ExposureProfileOut(**_profile(result["risk"])) if result["risk"] else None
        ),
        risk_unavailable=(
            "The real-world measure is estimated here for one interest rate. With a "
            "share it would take its expected return as well, which no data pins "
            "down: the profiles are those of the pricing measure."
            if asset_argument is not None
            else (
                None
                if foreign is None
                else "The real-world measure is estimated here for one interest rate. "
                "With two currencies it would take the joint history of two rates and "
                "of the exchange rate: the profiles are those of the pricing measure."
            )
        ),
        adjustments=_adjustments(result),
        adjustments_uncollateralised=_adjustments(open_set) if open_set else None,
        initial_margin=(
            InitialMarginOut(
                today=result["initial_margin"]["today"],
                times=result["exposure"]["times"],
                expected=result["initial_margin"]["expected"],
                requested=req.csa.initial_margin_model,
                model=margin_model,
                reason=margin_reason,
                simm_today=result["simm_today"],
            )
            if result["initial_margin"]
            else None
        ),
        # SA-CCR is margined when the counterparty posts variation margin.
        capital=_capital(
            result,
            capital,
            default_rate,
            margined=req.csa is not None,
            method_reason=capital_reason,
        ),
        capital_uncollateralised=(
            _capital(
                open_set,
                capital,
                default_rate,
                margined=False,
                method_reason=capital_reason,
            )
            if open_set
            else None
        ),
        market=XvaMarket(
            currency="USD",
            curve_as_of=rates.as_of,
            curve_label=rates.label,
            hull_white=HullWhiteInput(
                mean_reversion=rates.mean_reversion,
                sigma=rates.sigma,
                rmse_bp=rates.rmse_bp,
                vol_points=rates.vol_points,
                swaption_trades=rates.swaption_trades,
            ),
            counterparty=counterparty,
            own=own,
            recovery=req.recovery,
            historical=historical,
            default_rate=default_rate,
            foreign=_foreign_market(foreign) if foreign is not None else None,
            equity=(
                EquityMarket(
                    ticker=asset.ticker,
                    spot=asset.spot,
                    spot_as_of=asset.spot_as_of,
                    dividend_yield=asset.dividend_yield,
                    dividend_as_of=asset.dividend_as_of,
                    market_volatility=asset.market_volatility,
                    volatility_source=asset.volatility_source,
                    volatility_proxied=asset.volatility_proxied,
                    calibration_strike=asset.calibration_strike,
                    calibration_expiry=asset.calibration_expiry,
                    spot_volatility=asset_volatility,
                    correlation=CorrelationEstimate(
                        value=asset.correlation.value,
                        low=asset.correlation.low,
                        high=asset.correlation.high,
                        weeks=asset.correlation.weeks,
                    ),
                    correlation_series=[
                        xva_fx.USD_RATE_SERIES,
                        f"{asset.ticker} closes",
                    ],
                    correlation_start=asset.correlation_start,
                    correlation_end=asset.correlation_end,
                )
                if asset is not None and asset_argument is not None
                else None
            ),
        ),
        paths=req.paths,
        pilot_paths=result["pilot_paths"],
        seed=req.seed,
        device=result["device"],
        gpus=result["gpus"],
        device_reason=_device_reason(req.device, result),
        compute_ms=(time.perf_counter() - started) * 1000.0,
        warnings=warnings,
        methodology=methodology(
            two_currencies=foreign is not None, equity=asset_argument is not None
        ),
    )


def _foreign_market(m: xva_fx.TwoCurrencyMarket) -> ForeignMarket:
    def estimate(e: xva_fx.Estimate) -> CorrelationEstimate:
        return CorrelationEstimate(value=e.value, low=e.low, high=e.high, weeks=e.weeks)

    return ForeignMarket(
        currency=xva_fx.FOREIGN,
        curve_as_of=m.rates.as_of,
        curve_label=m.rates.label,
        hull_white=HullWhiteInput(
            mean_reversion=m.rates.mean_reversion,
            sigma=m.rates.sigma,
            rmse_bp=m.rates.rmse_bp,
            vol_points=m.rates.vol_points,
            swaption_trades=m.rates.swaption_trades,
        ),
        spot=m.spot,
        spot_as_of=m.spot_as_of,
        spot_volatility=m.spot_volatility,
        calibration_expiry=m.calibration_expiry,
        fx_volatilities=[
            FxVolatilityPoint(
                expiry=v.expiry,
                market=v.market,
                low=v.low,
                high=v.high,
                straddles=v.straddles,
                model=v.model,
            )
            for v in m.fx_volatilities
        ],
        fx_window_start=m.fx_window_start,
        correlation_rates=estimate(m.domestic_foreign),
        correlation_domestic_fx=estimate(m.domestic_fx),
        correlation_foreign_fx=estimate(m.foreign_fx),
        correlation_start=m.correlation_start,
        correlation_end=m.correlation_end,
        correlation_series=[
            xva_fx.USD_RATE_SERIES,
            xva_fx.EUR_RATE_SERIES,
            "ECB EUR/USD reference rate",
        ],
    )


def _device_reason(requested: str, result: dict) -> str:
    """Why the paths ran where they did, for the reader of the response."""
    if result["device"] == "gpu":
        cards = result["gpus"]
        return (
            f"Every trade has a closed form: the paths were valued on "
            f"{cards} GPU{'s' if cards > 1 else ''}, with the scenarios the CPU "
            "would have drawn."
        )
    if requested == "cpu":
        return "The CPU was requested."
    return f"On the CPU: {result['device_note']}."


#: The tenors at which SA-CVA asks for the sensitivity to the counterparty's
#: credit spread (Basel framework, MAR50.65).
_SPREAD_TENORS = (0.5, 1.0, 3.0, 5.0, 10.0)


def compute_sensitivities(
    req: XvaSensitivitiesRequest,
    today: Optional[date] = None,
    book: Optional[Book] = None,
) -> XvaSensitivitiesResponse:
    """CVA, DVA and FVA of the netting set differentiated to every quote the
    market gives, by adjoint differentiation, and the SA-CVA capital from
    those sensitivities. `book`: the trades of a portfolio's netting set, at
    the rates of their contracts (portfolio_xva.py)."""
    started = time.perf_counter()
    warnings: List[str] = []
    rates = _rates_market(today)
    label, _, specs, asset = _netting_set(req, book, rates, warnings)
    closed = {
        "swap",
        "swaption",
        "fx_forward",
        "cross_currency_swap",
        "equity_option",
        "cashflow",
    }
    by_regression = sorted({s.kind for s in specs} - closed)
    if not specs or by_regression:
        raise XvaInputError(
            "The sensitivities are computed for trades whose value on a path is a "
            "formula that can be differentiated: swaps, European swaptions, FX "
            "forwards, cross-currency swaps and European equity options. "
            + (
                f"This portfolio holds a {by_regression[0]}, valued by regression."
                if by_regression
                else (
                    "This netting set holds no such trade."
                    if book is not None
                    else "This portfolio is made of scripts, valued by regression."
                )
            )
        )
    if req.csa is not None and req.csa.initial_margin:
        raise XvaInputError(
            "The sensitivities cover variation margin; initial margin, which is a "
            "model of the margin fitted on the paths, is not differentiated."
        )
    counterparty = _credit(req.counterparty_rating, req.recovery, rates, warnings)
    own = _credit(req.own_rating, req.recovery, rates, warnings)
    # A second factor: a share (lot X14) or a second currency (lot X10). The
    # run is then the two-factor one, at a fixed market volatility of the
    # share or of the exchange rate.
    second: Optional[dict] = None
    second_name = ""
    if _has_asset(specs):
        if asset is None:
            raise XvaInputError("An equity option needs the market of its share.")
        trades, argument, _ = _asset_trades(specs, rates, asset)
        second_name = asset.ticker
        second = {
            "currency": asset.ticker,
            "spot": asset.spot,
            "dividend_yield": asset.dividend_yield,
            "market_volatility": asset.market_volatility,
            "volatility_expiry": asset.calibration_expiry,
            "rate_correlation": argument["rate_correlation"],
            "index": asset.index,
        }
    elif _two_currencies(specs):
        foreign = _two_currency_market(rates)
        trades = _two_currency_trades(specs, rates, foreign)
        second_name = "EUR/USD"
        quoted = min(
            foreign.fx_volatilities,
            key=lambda v: abs(v.expiry - foreign.calibration_expiry),
        )
        second = {
            **foreign.argument(),
            "market_volatility": quoted.market,
            "volatility_expiry": quoted.expiry,
        }
        second.pop("fx_volatility")
    else:
        trades = _trades(specs, rates)
    csa = _csa_argument(req)
    lgd = 1.0 - req.recovery
    horizon = [max(s.start + s.tenor for s in specs)]
    investment_grade = counterparty.rating in _INVESTMENT_GRADE
    try:
        result = _run_sensitivities(
            req, rates, trades, counterparty, own, horizon, lgd, csa, second
        )
    except RuntimeError as exc:
        if second is None:
            raise
        # The spot volatility is calibrated again in the run.
        raise XvaInputError(str(exc)) from exc
    if second is not None:
        warnings.append(
            f"The sensitivities to the rate model hold the market volatility of "
            f"{second_name} fixed, not its volatility in the model, which the "
            "calibration moves with the rates."
        )
        warnings.append(
            "The correlations are historical estimates: their sensitivities say "
            "what an error on them costs, not what can be hedged."
        )

    warnings.append(
        "Counterparty and own credit curves are rating proxies built from bond "
        "spreads (ICE BofA indices), not CDS quotes of a name: the same spread is "
        "used at every tenor."
    )
    if req.csa is not None and req.csa.minimum_transfer_amount > 0:
        warnings.append(
            "A minimum transfer amount makes the collateral jump; the sensitivities "
            "are those of the path-by-path estimator and leave these jumps out."
        )

    def risks(rows: List[dict]) -> List[AdjustmentRisks]:
        return [AdjustmentRisks(**row) for row in rows]

    sa = result["sa_cva"]
    # The model calls the share a foreign currency: say what it is.
    relabel = {
        "foreign zero rate, flat curve": f"dividend yield of {second_name}",
        "spot": (
            f"price of {second_name}" if _has_asset(specs) else f"{second_name} spot"
        ),
        "market volatility of the spot": f"market volatility of {second_name}",
        "correlation of the domestic rate and the spot": (
            f"correlation of the dollar rate and {second_name}"
        ),
        "correlation of the two rates": "correlation of dollar and euro rates",
        "correlation of the foreign rate and the spot": (
            f"correlation of the euro rate and {second_name}"
        ),
    }

    def move(row: dict) -> Tuple[float, str]:
        """The move a reader thinks in, for what is not a rate."""
        if row["label"] == "spot":
            return 0.01 * row["level"], "+1 %"
        if row["label"] == "market volatility of the spot":
            return 0.01, "+1 volatility point"
        if row["label"].startswith("correlation"):
            return 0.01, "+0.01"
        return 1e-4, "+1 bp"

    second_factor = [
        {
            "expiry": 0.0,
            **row,
            "label": relabel.get(
                row["label"], row["label"].replace("foreign", "euro", 1)
            ),
            "move": move(row)[0],
            "move_label": move(row)[1],
        }
        for row in result["second_factor"]
    ]
    return XvaSensitivitiesResponse(
        portfolio=req.portfolio,
        portfolio_label=label,
        adjustments=SensitivityAdjustments(**result["adjustments"]),
        swap_rates=risks(result["swap_rates"]),
        swaption_vols=risks(result["swaption_vols"]),
        counterparty_spreads=risks(result["counterparty_spreads"]),
        own_credit=risks(result["own_credit"]),
        others=risks(result["others"]),
        second_factor=risks(second_factor),
        model_risks=risks([{"expiry": 0.0, **row} for row in result["model_risks"]]),
        sa_cva=(
            SaCvaOut(**sa, sector=req.sector, investment_grade=investment_grade)
            if sa is not None
            else None
        ),
        sa_cva_unavailable=(
            None
            if sa is not None
            else "SA-CVA is computed here from the interest rate and counterparty "
            "credit spread sensitivities. This netting set also moves with "
            + ("a share" if _has_asset(specs) else "an exchange rate")
            + ", whose risk class the capital would have to count as well: it is "
            "not reported rather than understated."
        ),
        hull_white=HullWhiteInput(
            mean_reversion=result["hull_white"]["mean_reversion"],
            sigma=result["hull_white"]["sigma"],
            rmse_bp=rates.rmse_bp,
            vol_points=rates.vol_points,
            swaption_trades=rates.swaption_trades,
        ),
        market_as_of=rates.as_of,
        paths=req.paths,
        seed=req.seed,
        threads=result["threads"],
        inputs=result["factors"],
        seconds_adjoint=result["seconds_adjoint"],
        seconds_valuation=result["seconds_valuation"],
        cost_ratio=result["seconds_adjoint"] / max(result["seconds_valuation"], 1e-9),
        bump_valuations=2 * result["factors"],
        compute_ms=(time.perf_counter() - started) * 1000.0,
        warnings=warnings,
        methodology=sensitivities_methodology(second is not None),
    )


def _run_sensitivities(
    req, rates, trades, counterparty, own, horizon, lgd, csa, second
) -> dict:
    return qm.xva_sensitivities(
        rates.quotes,
        rates.vols,
        trades,
        # One spread per rating: the same at every tenor.
        [(t, counterparty.spread) for t in _SPREAD_TENORS],
        (horizon, [own.hazard]),
        req.recovery,
        lgd,
        lgd,
        csa,
        req.borrowing_spread,
        req.lending_spread,
        req.paths,
        req.seed,
        sector=req.sector,
        investment_grade=counterparty.rating in _INVESTMENT_GRADE,
        foreign=second,
    )


def market_inputs(response: XvaResponse) -> List[Tuple[str, str, str, object]]:
    """(name, source, as_of, value) of what the computation read from the
    store, for the audit record."""
    m = response.market
    default_rate = (
        [
            (
                f"default_rate:{m.default_rate.rating}",
                "db:credit.rating_default_rates",
                f"{m.default_rate.last_year}-12-31",
                m.default_rate.rate,
            )
        ]
        if m.default_rate is not None
        else []
    )
    return (
        [
            (
                "sofr_swap_curve",
                "db:rates.dtcc_swap_rates",
                m.curve_as_of.isoformat(),
                m.curve_label,
            ),
            (
                "hull_white_calibration",
                "db:rates.dtcc_swaptions",
                m.curve_as_of.isoformat(),
                [m.hull_white.mean_reversion, m.hull_white.sigma],
            ),
            (
                f"credit_spread:{m.counterparty.rating}",
                "db:macro.fred_series",
                m.counterparty.as_of.isoformat(),
                m.counterparty.spread,
            ),
            (
                f"credit_spread:{m.own.rating}",
                "db:macro.fred_series",
                m.own.as_of.isoformat(),
                m.own.spread,
            ),
        ]
        + (
            [
                (
                    f"rate_history:{m.historical.series}",
                    "db:macro.fred_series",
                    None,
                    [
                        m.historical.estimated_mean_reversion,
                        m.historical.estimated_long_run_rate,
                        m.historical.estimated_sigma,
                    ],
                )
            ]
            if m.historical is not None
            else []
        )
        + _foreign_inputs(m.foreign)
        + _equity_inputs(m.equity)
        + default_rate
    )


def _equity_inputs(e: Optional[EquityMarket]) -> List[Tuple[str, str, str, object]]:
    if e is None:
        return []
    as_of = e.spot_as_of.isoformat()
    return [
        (f"spot:{e.ticker}", "db:prices", as_of, e.spot),
        (f"dividend_yield:{e.ticker}", "db:prices", as_of, e.dividend_yield),
        (
            f"volatility:{e.ticker}",
            "db:options" if not e.volatility_proxied else "db:prices",
            as_of,
            [e.market_volatility, e.calibration_strike, e.calibration_expiry],
        ),
        (
            f"correlation:{xva_fx.USD_RATE_SERIES}:{e.ticker}",
            "db:fred+prices",
            e.correlation_end.isoformat(),
            e.correlation.value,
        ),
    ]


def _foreign_inputs(f: Optional[ForeignMarket]) -> List[Tuple[str, str, str, object]]:
    if f is None:
        return []
    as_of = f.curve_as_of.isoformat()
    return [
        ("euribor_swap_curve", "db:rates.dtcc_swap_rates", as_of, f.curve_label),
        (
            "hull_white_calibration:EUR",
            "db:rates.dtcc_swaptions",
            as_of,
            [f.hull_white.mean_reversion, f.hull_white.sigma],
        ),
        (
            "fx_spot:EURUSD",
            "db:fx.ecb_reference_rates",
            f.spot_as_of.isoformat(),
            f.spot,
        ),
        (
            "fx_volatility:EURUSD",
            "db:fx.dtcc_options",
            as_of,
            [f.calibration_expiry, f.spot_volatility],
        ),
        (
            "correlations:USD_EUR_EURUSD",
            "db:macro.fred_series,macro.intl_rates,fx.ecb_reference_rates",
            f.correlation_end.isoformat(),
            [
                f.correlation_rates.value,
                f.correlation_domestic_fx.value,
                f.correlation_foreign_fx.value,
            ],
        ),
    ]
