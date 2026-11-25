from datetime import date
from enum import Enum
from typing import Dict, List, Literal, Optional

from pydantic import BaseModel, Field, model_validator

from .util import today_utc


class EngineType(str, Enum):
    analytic = "analytic"
    mc = "mc"
    binomial = "binomial"
    trinomial = "trinomial"
    pde = "pde"


class ComputeDevice(str, Enum):
    """Where a Monte-Carlo pricing runs (blueprint/wp/19-gpu.md §8). `auto`
    takes the GPU when the server has one and the engine supports the request
    there, the CPU otherwise; `gpu` refuses to fall back."""

    cpu = "cpu"
    gpu = "gpu"
    auto = "auto"


class McRng(str, Enum):
    """Uniform generator of a CPU Monte-Carlo run. `philox` is the GPU's own
    generator: with it, CPU and GPU draw the same numbers and their prices
    agree to about 1e-15, so only the compute time differs."""

    pcg32 = "pcg32"
    philox = "philox"


class AmericanEngineType(str, Enum):
    binomial = "binomial"
    trinomial = "trinomial"
    #: Longstaff-Schwartz on a script, a Bermudan on 50 dates a year (#313):
    #: the engine the site offers; the trees stay for the portfolio's marks
    #: and as its oracle.
    mc = "mc"


class AsianAverageType(str, Enum):
    arithmetic = "arithmetic"
    geometric = "geometric"


class BarrierKind(str, Enum):
    up_and_in = "up-and-in"
    up_and_out = "up-and-out"
    down_and_in = "down-and-in"
    down_and_out = "down-and-out"


class DigitalPayoffKind(str, Enum):
    cash_or_nothing = "cash-or-nothing"
    asset_or_nothing = "asset-or-nothing"


class LookbackStyle(str, Enum):
    fixed_strike = "fixed-strike"
    floating_strike = "floating-strike"


class LookbackExtremum(str, Enum):
    minimum = "minimum"
    maximum = "maximum"


class VanillaRequest(BaseModel):
    spot: float = Field(..., gt=0.0)
    strike: float = Field(..., gt=0.0)
    maturity: float = Field(..., gt=0.0)
    rate: float
    dividend: float
    vol: float = Field(..., gt=0.0)
    is_call: bool
    is_american: bool = False
    engine: EngineType = EngineType.analytic
    # Capped so that a CPU run (~15 M paths/s on one core) stays inside the
    # 20 s pricing timeout: a timed-out request cannot stop its C++ thread.
    n_paths: int = Field(200000, ge=1, le=100_000_000)
    seed: int = 1
    mc_epsilon: float = 0.0
    tree_steps: int = Field(100, ge=10)
    pde_space_steps: int = Field(100, ge=10)
    pde_time_steps: int = Field(100, ge=10)
    device: ComputeDevice = Field(
        ComputeDevice.cpu,
        description="Monte-Carlo only: where the paths run. Ignored by the other engines.",
    )
    rng: McRng = Field(
        McRng.pcg32,
        description="Monte-Carlo on the CPU only; a GPU run always uses Philox.",
    )


class AmericanVanillaRequest(BaseModel):
    spot: float = Field(..., gt=0.0)
    strike: float = Field(..., gt=0.0)
    maturity: float = Field(..., gt=0.0)
    rate: float
    dividend: float
    vol: float = Field(..., gt=0.0)
    is_call: bool
    engine: AmericanEngineType = AmericanEngineType.binomial
    tree_steps: int = Field(100, ge=10)
    pde_space_steps: int = Field(100, ge=10)
    pde_time_steps: int = Field(100, ge=10)
    n_paths: int = Field(
        100_000,
        ge=1_000,
        le=2_000_000,
        description="engine='mc': pricing paths (the exercise rule's pilot is a quarter of them, 20,000 to 200,000).",
    )
    seed: int = Field(1, ge=1)
    exercise_per_year: int = Field(
        50,
        ge=1,
        le=252,
        description="engine='mc': Bermudan exercise dates a year standing for continuous exercise.",
    )


class AsianRequest(BaseModel):
    spot: float = Field(..., gt=0.0)
    strike: float = Field(..., gt=0.0)
    maturity: float = Field(..., gt=0.0)
    rate: float
    dividend: float
    vol: float = Field(..., gt=0.0)
    is_call: bool
    average_type: AsianAverageType = AsianAverageType.arithmetic
    engine: EngineType = EngineType.analytic
    n_paths: int = 200000
    seed: int = 1
    mc_epsilon: float = 0.0


class DatedAsianRequest(BaseModel):
    """Average-price Asian priced from calendar fixing dates through the
    timeline / day-count architecture (distinct from the legacy AsianRequest,
    which takes a single float maturity)."""

    spot: float = Field(..., gt=0.0)
    rate: float
    dividend: float = 0.0
    vol: float = Field(..., gt=0.0)
    valuation_date: date = Field(
        default_factory=today_utc,
        description="Time 0. Defaults to today (UTC) when omitted.",
    )
    fixing_dates: List[date] = Field(..., min_length=1)
    strike: float = Field(..., gt=0.0)
    is_call: bool = True
    geometric: bool = False
    day_count: Literal["ACT/365F", "ACT/360", "30/360", "ACT/ACT"] = "ACT/365F"
    sampler: Literal["pseudo", "sobol"] = "sobol"
    n_paths: int = Field(200_000, ge=1_000, le=5_000_000)
    seed: int = 1


class ScriptUnderlying(BaseModel):
    """One underlying of a multi-asset script (spot(i) is the i-th): a ticker
    whose inputs come from the database, an exchange rate (`fx`), or typed
    flat Black-Scholes inputs."""

    ticker: Optional[str] = Field(None, min_length=1)
    fx: Optional[str] = Field(
        None,
        pattern="^[A-Z]{3}$",
        description="An exchange rate as underlying: the price of one unit of "
        "this currency in the payment currency (ECB fixings), drifting at "
        "r_d - r_f. With a ticker listed in that currency, spot(i) * spot(j) "
        "is the asset in the payment currency: a composite.",
    )
    spot: Optional[float] = Field(None, gt=0.0)
    vol: Optional[float] = Field(None, gt=0.0)
    dividend: float = 0.0


class HullWhiteInputs(BaseModel):
    """The short rate of model='hull_white': Hull-White fitted to the
    `currency`'s government discount curve from the database, the equity
    correlated with it."""

    mean_reversion: float = Field(0.03, ge=1e-4, le=1.0)
    sigma: float = Field(
        0.01, gt=0.0, le=0.1, description="Short-rate vol, rate units (0.01 = 100bp)."
    )
    rho: float = Field(0.0, ge=-1.0, le=1.0, description="Equity-rates correlation.")
    currency: Literal["USD", "EUR", "GBP", "CHF", "JPY"] = "USD"
    curve: Literal["government", "sofr", "flat"] = Field(
        "government",
        description="'government': the currency's government curve, with the "
        "mean reversion and sigma given here. 'sofr' (USD only): the SOFR curve "
        "bootstrapped from the swaps DTCC published, and Hull-White calibrated "
        "to the ATM swaption vols of the trades -- mean_reversion and sigma are "
        "then the calibrated ones unless given explicitly. 'flat': a flat curve "
        "at the request's `rate`, mean reversion and sigma as given -- every "
        "input typed, nothing read from the database.",
    )

    @model_validator(mode="after")
    def _sofr_is_usd(self):
        if self.curve == "sofr" and self.currency != "USD":
            raise ValueError("curve='sofr' is the USD SOFR curve: currency must be USD")
        return self


class PrecisionTarget(BaseModel):
    """Price to a precision instead of a path count (issue 331, steps 2-3):
    a pilot run gives the per-path dispersion and the time-step bias, the
    path count follows, and the run stops at the first of four rules,
    which the response names (PrecisionOutcome.stopped)."""

    mode: Literal["absolute", "relative", "market"] = Field(
        "market",
        description="'absolute': `value` in the price's units. 'relative': "
        "`value` as a share of the price (0.001 = 0.1 %). 'market': k x |vega| "
        "x delta_sigma, delta_sigma the vanillas' half bid-ask spread in vol "
        "at the product's horizon (the tighter of call and put, median over "
        "near-the-money strikes of the stored chain), k = `value`: numerical "
        "error kept to a share of what the market itself knows.",
    )
    value: float = Field(
        0.25,
        gt=0.0,
        description="The target (absolute or relative), or k in 'market' mode "
        "(0.25 by default: a quarter of the market's half-spread).",
    )
    time_budget_s: float = Field(
        8.0,
        gt=0.0,
        le=18.0,
        description="Hard budget for the whole pricing: a run predicted beyond "
        "it is cut to what fits, and the response says how many paths the "
        "target needed. At most 18 s: the pricing routes answer 408 after 20 "
        "s of computing, and a precision target plans within 90 % of that.",
    )


class ScriptPricingInputs(BaseModel):
    """How a payoff script is priced (blueprint/wp/16-scripting.md), whatever
    the script: model, market, underlyings, Monte-Carlo settings. The script
    comes from ScriptRequest (typed) or ScriptedProductRequest (a library
    product and its terms). Priced by the generic Monte-Carlo
    engine. `fuzzy` smooths comparisons for a usable pathwise delta on
    digitals and barriers; discrete tests (flags) stay crisp either way.

    A script only describes a payoff; the dynamics its price depends on come
    from `model`. "auto" (the default) picks the simplest model that captures
    what the script's price depends on (scripting/model_advice.hpp
    recommend()): local vol for a payoff on each date's spot, stochastic-
    local vol for one carrying state across dates, flat Black-Scholes when no
    `ticker` is given. The response's `model_choice` says which and why.
    "black_scholes" takes `spot`, `vol` (one flat volatility). "local_vol",
    "heston" and "slv" take a `ticker` and price against the stored market
    data in the database data-ingest fills (option-chain snapshot, close,
    dividend yield) -- never a live source. A stored snapshot is a market
    date: the script is priced on the latest snapshot on or before
    `valuation_date` (at most a few days earlier), and the response says so.
    Missing or stale stored data is an error naming what to refresh. Either
    way, `warnings` in the response reports what the script's price depends
    on that the chosen model cannot capture."""

    model: Literal[
        "auto",
        "black_scholes",
        "local_vol",
        "heston",
        "slv",
        "rough_bergomi",
        "hull_white",
    ] = Field(
        "auto",
        description=(
            "'auto': the model the script needs, among those the inputs allow "
            "(a ticker for the market models, else spot and vol for flat "
            "Black-Scholes); announced in the response's model_choice. "
            "'black_scholes': flat vol (spot and vol, or a ticker: then the "
            "at-the-money implied vol of its stored smile at the script's last "
            "date). 'local_vol': "
            "Dupire surface calibrated from the ticker's stored option-chain "
            "snapshot. 'heston': Heston calibrated to that surface. 'slv': "
            "stochastic-local vol, the calibrated Heston times a leverage that "
            "reprices the surface. 'rough_bergomi': rough Bergomi (H, eta, rho) "
            "calibrated to the short end of that surface, on its variance-swap "
            "forward variance curve. The market models need a ticker; spot and "
            "dividend then come from the database. 'hull_white': the equity at a "
            "flat vol (as black_scholes) under stochastic Hull-White rates fitted "
            "to the government curve of `hull_white.currency`; df() in the script "
            "reads the simulated curve (equity-rates hybrids, rates products)."
        ),
    )
    hull_white: Optional[HullWhiteInputs] = Field(
        None, description="Parameters of model='hull_white' (defaults when omitted)."
    )
    ticker: Optional[str] = Field(
        None,
        min_length=1,
        description="Underlying whose stored option chain is calibrated (every model but black_scholes); must be in data-ingest's tracked universe.",
    )
    underlyings: Optional[List[ScriptUnderlying]] = Field(
        None,
        min_length=1,
        max_length=8,
        description=(
            "For a script that reads spot(1), spot(2)...: one entry per "
            "underlying, in spot(i) order. Either every entry has a ticker or "
            "an `fx` currency (spot, dividend, at-the-money implied vol and "
            "historical correlation from the database) or every entry has spot "
            "and vol (then `correlation` is required). Priced with each asset "
            "on the Dupire surface of its stored option chain when every ticker "
            "has one (local vol, drivers correlated), else under correlated "
            "Black-Scholes; replaces ticker/spot/vol/dividend. A single entry "
            "prices a one-asset script in another `currency` (a quanto)."
        ),
    )
    currency: Optional[str] = Field(
        None,
        pattern="^[A-Z]{3}$",
        description="Payment currency of a script on `underlyings` read from "
        "the database; `rate` is its rate. An asset listed in another currency "
        "is simulated under this currency's measure: its drift is lowered by "
        "r_d - r_f + rho sigma_S sigma_X (the quanto adjustment). Defaults to "
        "the first ticker's listing currency.",
    )
    correlation: Optional[List[List[float]]] = Field(
        None,
        description="n x n correlation matrix of the typed underlyings "
        "(ignored with tickers, whose correlation is historical).",
    )
    steps_per_year: Optional[int] = Field(
        None,
        ge=12,
        le=504,
        description="Time steps per year for local_vol, heston and slv "
        "(ignored by black_scholes, which is simulated exactly). None, the "
        "default: 64 for heston, whose price runs step the variance by QE, "
        "nearly exact over a long step (WP 26 §20); 252 for slv, whose "
        "leverage is held over each step (on SPY's fit of 2026-11-19, 64 a "
        "year put a 1y put 2 % above 504 a year, 252 within its noise), and "
        "for local vol (on SPY's grid a one-year ATM call, about 54, is 0.57 "
        "above its fine-step limit at 52 steps a year, 0.13 at 252, 0.02 at "
        "2016, blueprint WP 26 §9.3). The standard error does not include "
        "this discretisation bias; discretisation_bias estimates it, "
        "mlmc_eps controls it.",
    )
    spot: Optional[float] = Field(
        None,
        gt=0.0,
        description="Required for model='black_scholes' (and 'auto' without a ticker).",
    )
    rate: float
    dividend: float = 0.0
    vol: Optional[float] = Field(
        None,
        gt=0.0,
        description="Required for model='black_scholes' (and 'auto' without a ticker).",
    )
    valuation_date: date = Field(
        default_factory=today_utc,
        description="Time 0. Defaults to today (UTC) when omitted.",
    )
    day_count: Literal["ACT/365F", "ACT/360", "30/360", "ACT/ACT"] = "ACT/365F"
    fuzzy: bool = False
    default_eps: float = Field(0.01, gt=0.0)
    sampler: Literal["pseudo", "sobol", "stratified"] = Field(
        "sobol",
        description="'sobol' (the default: randomised QMC with a Brownian "
        "bridge, CPU and GPU, prices and AAD risks, the error from 16 "
        "independent replicates), 'pseudo' (antithetic pairs) or 'stratified': "
        "the terminal value of the "
        "first Brownian factor in equiprobable strata, the path filled in by the "
        "conditional Brownian bridge, the error from 16 independent replicates "
        "(blueprint WP 19 §2.5).",
    )
    control_variate: bool = Field(
        False,
        description="Regress the deflated spots at up to 8 event dates -- "
        "martingales of known mean -- out of the payoff (blueprint WP 19 §2.5). "
        "The diagnostics give the variance removed. Most useful without "
        "antithetic pairs, which already cancel the part linear in the spot.",
    )
    antithetic: bool = Field(
        True,
        description="Pair each path with its mirror (-z). Price only: the "
        "adjoint engines run plain paths.",
    )
    importance_sampling: bool = Field(
        False,
        description="Shift every Brownian factor by the drift at the mode of "
        "payoff x density (Glasserman, Heidelberger & Shahabuddin) and weight "
        "each path by its likelihood ratio; kept only when a pilot shows the "
        "variance falls, which the diagnostics report. For rare events (a "
        "deep out-of-the-money digital). Price only.",
    )
    n_paths: int = Field(200_000, ge=1_000, le=5_000_000)
    precision: Optional[PrecisionTarget] = Field(
        None,
        description="Price to this precision instead of n_paths (which is then "
        "ignored): pilot, path count, stopping rules (PrecisionTarget).",
    )
    seed: int = 1
    device: Optional[ComputeDevice] = Field(
        None,
        description="Where the paths run (blueprint WP 19 §8). Omitted (the "
        "default): 'auto' for a multilevel price and its local-vol AAD greeks "
        "(WP 26 §10: the GPU when the server has one, the CPU otherwise, said "
        "in the diagnostics), 'cpu' for every other run. The GPU prices the "
        "compiled script under Black-Scholes, local vol, Heston or SLV, "
        "pseudo-random, stratified or Sobol; with greeks_method='aad' it computes the "
        "risks under Black-Scholes, Heston (forward-mode duals) and local vol "
        "(per-path adjoint). 'auto' falls back to the CPU otherwise and says why "
        "in the diagnostics, 'gpu' refuses.",
    )
    rng: McRng = Field(
        McRng.pcg32,
        description="CPU runs only; a GPU run always uses Philox, the generator "
        "with which a CPU run gives the GPU's price.",
    )
    mlmc: Literal["auto", "on", "off"] = Field(
        "auto",
        description="'auto' (the default): multilevel Monte Carlo over the time "
        "step for a local_vol script that suits it -- one payoff, no early "
        "exercise -- to a root-mean-square error of "
        "mlmc_rel_eps of the price, or mlmc_eps when given; the usual engine "
        "otherwise, the diagnostics saying why. With greeks_method='aad' the "
        "risks come from the same levels (each level's adjoint, fine minus "
        "coarse) on the GPU, device 'gpu' or 'auto'; on the CPU they come "
        "from the usual engine (WP 26 §10.2). 'on': also under heston and "
        "slv, whose levels couple poorly (on SPY, 12 to 160 s on the CPU "
        "against about 2 s for the usual engine with its bias estimated; "
        "blueprint WP 26 §9.5). 'off': the usual engine (§9.4).",
    )
    mlmc_rel_eps: float = Field(
        0.002,
        gt=0.0,
        le=0.1,
        description="Multilevel target as a share of the price (0.2 % by "
        "default), split half to the standard error, half to the "
        "discretisation bias.",
    )
    bias_estimate: bool = Field(
        True,
        description="Estimate and return the discretisation bias of a plain "
        "(non-multilevel) run: the steps are halved and the bias read from "
        "the difference, at about three times the cost of one run of a "
        "time-stepped model.",
    )
    mlmc_eps: float = Field(
        0.0,
        ge=0.0,
        description="An absolute multilevel target instead of mlmc_rel_eps (0, "
        "the default: the relative one). The levels are refined until the "
        "estimated discretisation bias is below the target / sqrt(2). With "
        "sampler='sobol' (the default) the levels are Sobol points, multilevel "
        "QMC (WP 26 §9), on the GPU with device 'gpu' or 'auto' (each level's "
        "fine and coarse grids as two runs of the compiled script on the same "
        "points, WP 26 §10); with 'pseudo', multilevel Monte Carlo on the "
        "CPU. Price only; n_paths is then ignored.",
    )
    milstein: bool = Field(
        False,
        description="model='local_vol': the Milstein step instead of "
        "log-Euler (strong order 1; with mlmc_eps, the levels' variance "
        "decays as h^2 on a smooth surface). CPU only. Not advised on a "
        "calibrated market grid: where the local vol's slope is steep at the "
        "edge of the quoted strikes, it makes the levels' coupling worse "
        "(blueprint WP 26 §9.2).",
    )
    greeks_method: Literal["none", "aad"] = Field(
        "none",
        description=(
            "'aad': every model parameter's sensitivity (spot, rate, div, vol; "
            "under hull_white also the short rate's sigma and the curve, per "
            "quote or pillar) from one adjoint Monte-Carlo run "
            "(blueprint/wp/17-aad.md), at "
            "roughly 3-5x the cost of the price alone rather than a bumped "
            "reprice per parameter. 'bump' is not offered for scripted "
            "payoffs -- only 'none' or 'aad'. With sampler='sobol' the "
            "adjoint engines run Sobol RQMC with the Brownian bridge; "
            "'stratified' falls back to pseudo-random, noted in the "
            "response's diagnostics. On the GPU (device 'gpu' or 'auto') the "
            "same risks, on the same paths as the CPU tape with "
            "rng='philox' (blueprint WP 19 §6)."
        ),
    )
    second_order: bool = Field(
        False,
        description="With greeks_method='aad': also gamma, vanna and volga, "
        "each with its standard error (blueprint/wp/27-ir-ad.md §14.10-14.11): "
        "central differences of first-order risks at the spot bumped by 1 % "
        "and the vol by 25 bp (a parallel shift of a local-vol grid), each pair "
        "on the same points. The first-order risks are the GPU's adjoint, or, "
        "for a hard payoff that jumps at its final date (a digital), Vibrato "
        "(pathwise up to the last step, a likelihood ratio over it; "
        "Black-Scholes or local vol, the GPU or else the CPU). Vanna and volga "
        "need a model with one vol. About four first-order runs more. What "
        "cannot be covered (a jump at an earlier date, no GPU for the adjoint) "
        "is said in the diagnostics and the first-order answer stands.",
    )

    @model_validator(mode="after")
    def _model_inputs_present(self) -> "ScriptPricingInputs":
        if self.underlyings is not None:
            return self._multi_asset_inputs()
        flat_inputs = self.spot is not None and self.vol is not None
        if self.model in ("black_scholes", "hull_white"):
            if not flat_inputs and self.ticker is None:
                raise ValueError(
                    f"model='{self.model}' requires spot and vol, or a ticker "
                    "(then the at-the-money implied vol of its stored smile)"
                )
        elif self.model == "auto":
            if self.ticker is None and not flat_inputs:
                raise ValueError(
                    "model='auto' requires a ticker (market models) or spot and "
                    "vol (flat Black-Scholes)"
                )
        elif self.ticker is None:
            raise ValueError(f"model='{self.model}' requires a ticker")
        return self

    def _multi_asset_inputs(self) -> "ScriptPricingInputs":
        u = self.underlyings or []
        if self.model not in ("auto", "black_scholes", "local_vol"):
            raise ValueError(
                f"model='{self.model}' is single-underlying; several "
                "underlyings are priced under correlated Black-Scholes or local "
                "vol (model='auto', 'black_scholes' or 'local_vol')"
            )
        if any(x.ticker is not None and x.fx is not None for x in u):
            raise ValueError(
                "underlyings: an entry is a ticker or an fx rate, not both"
            )
        stored = [x.ticker is not None or x.fx is not None for x in u]
        if all(stored):
            return self
        if any(stored) or not all(x.spot and x.vol for x in u):
            raise ValueError(
                "underlyings: give every entry a ticker (or an fx currency), or "
                "every entry a spot and a vol"
            )
        if self.currency is not None:
            raise ValueError(
                "currency applies to underlyings read from the database; typed "
                "underlyings carry their drift in their dividend"
            )
        if len(u) < 2:
            raise ValueError("one typed underlying: give spot and vol instead")
        n = len(u)
        c = self.correlation
        if c is None or len(c) != n or any(len(row) != n for row in c):
            raise ValueError(f"typed underlyings need an {n} x {n} correlation")
        return self


class ScriptRequest(ScriptPricingInputs):
    """Price a payoff described in text."""

    script: str = Field(..., min_length=1, description="The script source text.")


class ScriptedProductRequest(ScriptPricingInputs):
    """Price a product of the script library (api/app/product_library) from
    its term sheet: the terms replace the defaults the script declares. An
    equity product starts on the valuation date, its first fixing today's
    spot (levels in % of the initial level are % of today's spot); a rates
    product a few days later. The response carries the script actually
    priced."""

    product: str = Field(
        ..., min_length=1, description="Library slug, e.g. 'worst-of-autocall'."
    )
    terms: Dict[str, float] = Field(
        default_factory=dict,
        description="Term name -> value (percent terms as fractions: 0.6 = 60 %). Missing terms take the product's defaults.",
    )
    n_paths: int = Field(50_000, ge=1_000, le=5_000_000)
    maturity: Optional[float] = Field(
        None,
        gt=0.0,
        le=30.0,
        description="Years from the start to the last date: the script's dates "
        "stretched in proportion, a schedule keeping its frequency. For a swap "
        "or a swaption, the swap's length in whole years. None: the product's own.",
    )
    expiry: Optional[float] = Field(
        None,
        gt=0.0,
        le=30.0,
        description="A swaption's (first) exercise, in years. None: one year.",
    )
    engine: Literal["mc", "analytic"] = Field(
        "mc",
        description=(
            "'mc': the script under `model` (Monte-Carlo). 'analytic': the "
            "closed form, for the products that have one (the European and "
            "digital calls and puts, the forward): Black-Scholes at the "
            "at-the-money implied vol of the ticker's stored smile."
        ),
    )

    @model_validator(mode="before")
    @classmethod
    def _rates_market(cls, data):
        """A rates product of the library (`# market: rates`: bonds, swaps)
        reads no stock, only the curve: it is priced by Hull-White fitted to
        `hull_white.currency`'s government curve. The hybrid model also
        carries a stock, which the script never reads: it is given a unit
        spot and no volatility here, so that no stock input is asked for."""
        if not isinstance(data, dict):
            return data
        from . import product_templates

        try:
            template = product_templates.get(str(data.get("product", "")))
        except ValueError:
            return data
        if template.market != "rates":
            return data
        if data.get("ticker") or data.get("underlyings"):
            raise ValueError(
                f"'{template.slug}' is a rates product: it reads the curve of "
                "`hull_white.currency`, not a ticker"
            )
        if data.get("model", "auto") not in ("auto", "hull_white"):
            raise ValueError(
                f"'{template.slug}' is a rates product, priced by Hull-White on "
                "the currency's curve (model='hull_white')"
            )
        hull_white = data.get("hull_white")
        hw_asked = (
            hull_white.model_dump(exclude_unset=True)
            if isinstance(hull_white, BaseModel)
            else dict(hull_white or {})
        )
        if hw_asked.get("curve") == "flat":
            # every input typed: a flat curve at `rate`, the request's a and sigma
            pass
        elif template.curve == "sofr":
            # the SOFR market of the traded swaps and swaptions, USD only
            hw = (
                hull_white.model_dump(exclude_unset=True)
                if isinstance(hull_white, BaseModel)
                else dict(hull_white or {})
            )
            if hw.get("currency", "USD") != "USD" or hw.get("curve", "sofr") != "sofr":
                raise ValueError(
                    f"'{template.slug}' is priced on the USD SOFR curve of the traded "
                    "swaps (hull_white.curve='sofr', currency USD), or on a flat "
                    "typed curve (hull_white.curve='flat')"
                )
            hull_white = {**hw, "currency": "USD", "curve": "sofr"}
        # `rate` is unread too: the curve discounts
        return {
            **data,
            "hull_white": hull_white,
            "model": "hull_white",
            "spot": 1.0,
            "vol": 1e-12,
            "dividend": 0.0,
            "rate": data.get("rate", 0.0),
        }


class ScriptValidateRequest(BaseModel):
    """Parse a script and resolve its timeline, without pricing it — no
    market inputs needed. For an editor's "Validate" action: instant, and
    surfaces a malformed script before a 200k-path simulation is even
    considered."""

    script: str = Field(..., min_length=1, description="The script source text.")
    valuation_date: date = Field(
        default_factory=today_utc,
        description="Time 0. Defaults to today (UTC) when omitted.",
    )
    day_count: Literal["ACT/365F", "ACT/360", "30/360", "ACT/ACT"] = "ACT/365F"
    start_today: bool = Field(
        False,
        description="Read the script as the Pricing page prices a library "
        "product: its first date moved to the valuation date and fixed there, "
        "so levels struck on it are known (a European, not a forward start). "
        "The events returned are then the dates still to come.",
    )


class ScriptEvent(BaseModel):
    date: date
    t: float = Field(..., description="Year-fraction from the valuation date.")


class ScriptAnalysis(BaseModel):
    """Structural facts read off the script that decide which model dynamics
    its price depends on -- never a numerical estimate."""

    n_underlyings: int
    nonlinear_in_spot: bool
    spot_threshold_test: bool
    spot_comparison: bool = Field(
        False,
        description="A crisp comparison of a spot-derived value: unless fuzzy, "
        "the payoff jumps there along a path. Unlike spot_threshold_test, not "
        "set by the bridge survivals or lastStepDigital, whose values are smooth.",
    )
    path_dependent: bool


class ModelRecommendation(BaseModel):
    """The model 'auto' picks for a script, and why (scripting/model_advice.hpp
    recommend())."""

    model: Literal["black_scholes", "local_vol", "heston", "slv"]
    code: str = Field(..., description="Stable key of the reason.")
    reason: str


class ScriptValidateResponse(BaseModel):
    events: List[ScriptEvent]
    variables: List[str]
    analysis: ScriptAnalysis
    recommendation: ModelRecommendation = Field(
        ...,
        description="What model='auto' picks when the ticker's surface and its "
        "stochastic-vol calibration are available (the usual case); the "
        "pricing response's model_choice reports the actual choice.",
    )


class BarrierRequest(BaseModel):
    spot: float = Field(..., gt=0.0)
    strike: float = Field(..., gt=0.0)
    maturity: float = Field(..., gt=0.0)
    rate: float
    dividend: float
    vol: float = Field(..., gt=0.0)
    is_call: bool
    barrier_level: float = Field(..., gt=0.0)
    barrier_kind: BarrierKind
    rebate: float = 0.0
    # Barrier MC runs 9 path-variants per path (CRN Greek scheme);
    # 50 000 paths keeps latency under ~2 s.  Raise for more precision.
    n_paths: int = 50000
    seed: int = 1
    mc_epsilon: float = 0.0
    # 0 = auto (50 steps/yr).  Pass > 0 to override.
    n_steps: int = 0
    brownian_bridge: bool = True


class DigitalRequest(BaseModel):
    spot: float = Field(..., gt=0.0)
    strike: float = Field(..., gt=0.0)
    maturity: float = Field(..., gt=0.0)
    rate: float
    dividend: float
    vol: float = Field(..., gt=0.0)
    is_call: bool
    payoff_type: DigitalPayoffKind = DigitalPayoffKind.cash_or_nothing
    cash_amount: float = Field(1.0, gt=0.0)


class LookbackRequest(BaseModel):
    spot: float = Field(..., gt=0.0)
    strike: float = Field(..., gt=0.0)
    maturity: float = Field(..., gt=0.0)
    rate: float
    dividend: float
    vol: float = Field(..., gt=0.0)
    is_call: bool
    style: LookbackStyle = LookbackStyle.fixed_strike
    # extremum is only relevant for fixed-strike; ignored for floating-strike.
    extremum: LookbackExtremum = LookbackExtremum.maximum
    n_steps: int = 0  # 0 = auto (252 × T steps)
    n_paths: int = 200000
    seed: int = 1
    mc_antithetic: bool = True
    mc_epsilon: float = 0.0


class BasketRequest(BaseModel):
    # Per-asset parameters (all lists must have the same length n ≥ 2)
    spots: List[float] = Field(..., min_length=2)
    vols: List[float] = Field(..., min_length=2)
    dividends: List[float] = Field(default_factory=list)
    weights: List[float] = Field(default_factory=list)
    # Uniform pairwise correlation ρ ∈ (-1, 1).
    # The full n×n correlation matrix is C[i][j] = ρ for i≠j, 1 for i=j.
    pairwise_correlation: float = Field(0.0, ge=-0.999, le=0.999)
    # Basket-level parameters
    strike: float = Field(..., gt=0.0)
    maturity: float = Field(..., gt=0.0)
    rate: float
    is_call: bool
    n_paths: int = 200000
    seed: int = 1
    mc_antithetic: bool = True


class FutureRequest(BaseModel):
    spot: float = Field(..., gt=0.0)
    strike: float = Field(..., gt=0.0)
    maturity: float = Field(..., gt=0.0)
    rate: float
    dividend: float
    notional: float = 1.0


class ZeroCouponBondRequest(BaseModel):
    maturity: float = Field(..., gt=0.0)
    rate: float
    notional: float = 1.0
    discount_times: List[float] = Field(default_factory=list)
    discount_factors: List[float] = Field(default_factory=list)


class FixedRateBondRequest(BaseModel):
    maturity: float = Field(..., gt=0.0)
    rate: float
    coupon_rate: float = Field(..., ge=0.0)
    coupon_frequency: int = Field(1, ge=1)
    notional: float = 1.0
    discount_times: List[float] = Field(default_factory=list)
    discount_factors: List[float] = Field(default_factory=list)


class Greeks(BaseModel):
    delta: Optional[float] = None
    gamma: Optional[float] = None
    vega: Optional[float] = None
    theta: Optional[float] = None
    rho: Optional[float] = None

    delta_std_error: Optional[float] = None
    gamma_std_error: Optional[float] = None
    vega_std_error: Optional[float] = None
    theta_std_error: Optional[float] = None
    rho_std_error: Optional[float] = None

    # Second order of a script (second_order=True, WP 27 §14.10):
    # d delta / d vol and d vega / d vol.
    vanna: Optional[float] = None
    volga: Optional[float] = None
    vanna_std_error: Optional[float] = None
    volga_std_error: Optional[float] = None


class BondAnalytics(BaseModel):
    macaulay_duration: Optional[float] = None
    modified_duration: Optional[float] = None
    convexity: Optional[float] = None
    dv01: Optional[float] = None


class RiskEntry(BaseModel):
    """One model parameter's AAD sensitivity (blueprint/wp/17-aad.md §13.1).
    Present only when priced with greeks_method="aad" -- unlike Greeks'
    five fixed slots, a future model with a local-vol grid or a multi-asset
    correlation matrix reports here regardless of how many parameters it
    has."""

    label: str
    value: float
    std_error: float


class ModelWarning(BaseModel):
    """Something the script's price depends on that the chosen model cannot
    capture (scripting/model_advice.hpp)."""

    code: str
    severity: Literal["warning", "info"]
    message: str


class HestonFit(BaseModel):
    """Heston calibrated to the stored surface (market/heston_calibration.hpp).
    iv_rmse / iv_worst: exact implied-vol errors over the fitted points, in
    vol (0.01 = one vol point)."""

    v0: float
    kappa: float
    theta: float
    xi: float
    rho: float
    iv_rmse: float
    iv_worst: float
    n_quotes: int
    n_maturities: int
    feller: bool = Field(..., description="2 kappa theta > xi^2.")


class LeverageFit(BaseModel):
    """The SLV leverage L(K, T) on the Dupire grid (market/slv_calibration.hpp)."""

    min: float
    max: float
    clamped_share: float = Field(
        ...,
        description="Share of grid points held at the calibration's floor or "
        "cap, where the marginals are not the surface's.",
    )
    clamped_mass: Optional[float] = Field(
        None,
        description="Share of the calibration's particles, over every column, "
        "in those points: how much of the distribution is not matched.",
    )
    n_particles: int


class RoughBergomiFit(BaseModel):
    """Rough Bergomi calibrated to the short end of the stored surface
    (market/rough_bergomi_calibration.hpp). iv_rmse / iv_worst in vol;
    mc_vol_error: the largest Monte-Carlo error of a model vol, the
    resolution below which the fit cannot tell parameters apart."""

    H: float
    eta: float
    rho: float
    xi_times: List[float]
    xi_values: List[float] = Field(
        ..., description="Forward variance xi0(t) from the variance-swap curve."
    )
    xi_floored: int = Field(
        ..., description="Forward variances floored (calendar arbitrage)."
    )
    iv_rmse: float
    iv_worst: float
    n_quotes: int
    n_maturities: int
    mc_vol_error: float


class ModelCalibration(BaseModel):
    ticker: str
    snapshot: date
    heston: Optional[HestonFit] = None
    leverage: Optional[LeverageFit] = None
    rough_bergomi: Optional[RoughBergomiFit] = None
    flat_vol: Optional[float] = Field(
        None,
        description="Flat Black-Scholes on a ticker: the at-the-money implied "
        "vol of the stored smile at the script's last date.",
    )
    rates_curve: Optional[str] = Field(
        None,
        description="model='hull_white': the discount curve the short rate is "
        "fitted to, and its date.",
    )
    seconds: float = Field(
        ..., description="Wall time of the calibration (cached per snapshot)."
    )


class UnderlyingUsed(BaseModel):
    """What one underlying of a multi-asset script was priced with."""

    ticker: Optional[str] = None
    fx: Optional[str] = Field(
        None, description="An exchange-rate underlying's currency."
    )
    currency: Optional[str] = Field(None, description="The currency the spot is in.")
    spot: float
    dividend: float
    vol: float
    vol_source: str = Field(..., description="Where the vol comes from.")
    drift_adjustment: float = Field(
        0.0,
        description="Added to the dividend yield in the simulation: r_f for an "
        "exchange rate, r_d - r_f + rho sigma_S sigma_X for an asset listed in "
        "another currency than the payment one (quanto), 0 otherwise.",
    )
    drift_source: Optional[str] = None


class SwaptionVolFit(BaseModel):
    """One ATM swaption vol Hull-White was calibrated to."""

    expiry: float
    tenor: float
    market_vol: float = Field(..., description="Median implied normal vol, rate units.")
    trades: int


class HullWhiteRatesFit(BaseModel):
    """Hull-White calibrated to the traded swaptions (hull_white.curve =
    'sofr'): the curve, the parameters and how well they fit."""

    curve: str = Field(..., description="The curve, its date and its source.")
    as_of: date
    window_start: date = Field(
        ..., description="First day of the swaption trades used."
    )
    mean_reversion: float
    sigma: float
    calibrated: bool = Field(
        ..., description="False when the request gave mean_reversion and sigma itself."
    )
    rmse_bp: float = Field(
        ..., description="Root-mean-square fit error, bp of normal vol."
    )
    worst_bp: float
    converged: bool
    points: List[SwaptionVolFit]
    trades_used: int
    seconds: float


class ModelChoice(BaseModel):
    """The model a scripted payoff was priced under: requested, chosen, why,
    and what was calibrated for it."""

    requested: Literal[
        "auto",
        "black_scholes",
        "local_vol",
        "heston",
        "slv",
        "rough_bergomi",
        "hull_white",
    ]
    model: Literal[
        "black_scholes", "local_vol", "heston", "slv", "rough_bergomi", "hull_white"
    ]
    code: str = Field(
        ..., description="Stable key of the reason ('user' when chosen by hand)."
    )
    reason: str
    calibration: Optional[ModelCalibration] = None
    underlyings: Optional[List[UnderlyingUsed]] = Field(
        None, description="Multi-asset scripts: each underlying, in spot(i) order."
    )
    correlation: Optional[List[List[float]]] = None
    correlation_source: Optional[str] = None
    rates_calibration: Optional[HullWhiteRatesFit] = Field(
        None,
        description="hull_white.curve = 'sofr': the SOFR curve and Hull-White "
        "calibrated to the traded swaptions.",
    )


class ComputeTimings(BaseModel):
    """compute_ms split by phase. The phases are exclusive: market + calibration
    + engine + other = compute_ms. greeks_ms comes on top of it."""

    market_ms: float = Field(
        ...,
        description="Reading market data from the database (prices, option "
        "chain, dividend, curves) and preparing it.",
    )
    calibration_ms: float = Field(
        ...,
        description="Fitting the model to that data: the implied-vol surface "
        "and Dupire grid, Heston / SLV leverage, rough Bergomi, Hull-White to "
        "swaptions. Near zero when the in-process cache holds it.",
    )
    engine_ms: float = Field(
        ...,
        description="The numerical method alone: the Monte-Carlo run(s) with "
        "their adjoint greeks, the tree, the PDE or the closed form.",
    )
    other_ms: float = Field(
        ...,
        description="What no phase covers: script parsing, model choice, "
        "Python glue.",
    )
    greeks_ms: float = Field(
        0.0,
        description="The greeks the engine did not give, by bumping and "
        "revaluing (bumped_greeks): each revaluation reloads its market. Not in "
        "compute_ms.",
    )


class PrecisionOutcome(BaseModel):
    """How a pricing to a precision ended (PrecisionTarget)."""

    mode: Literal["absolute", "relative", "market"]
    target: float = Field(..., description="The error aimed at, in the price's units.")
    stopped: Literal["target_reached", "out_of_budget", "bias_floor", "time_budget"] = (
        Field(
            ...,
            description="target_reached: sqrt(se^2 + bias^2) within the target. "
            "out_of_budget: the pilot predicted more time than the budget; the run "
            "used what fits. bias_floor: the time-step bias alone is at least "
            "target / sqrt(2) -- more paths cannot help, finer steps or multilevel "
            "would. time_budget: the run reached the budget before the target.",
        )
    )
    paths: int = Field(
        ...,
        description="Paths the engine simulated in the run returned: under "
        "Sobol, the replicates times the power of two nearest the points asked "
        "per replicate, not the count the precision engine asked for.",
    )
    paths_needed: Optional[int] = Field(
        None, description="Paths the pilot predicted for the target's half of variance."
    )
    achieved_error: float = Field(
        ..., description="sqrt(std_error^2 + bias^2), the bias when resolved, else 0."
    )
    pilot_paths: int
    seconds: float = Field(..., description="Wall time of the pilot and the run.")
    delta_sigma: Optional[float] = Field(
        None, description="'market': the vanillas' half-spread in vol at the horizon."
    )
    vega_scale: Optional[float] = Field(
        None,
        description="'market': d price / d (relative scale of every vol), by "
        "a 1 % bump either way on the same draws.",
    )
    atm_vol: Optional[float] = Field(
        None, description="'market': the ATM implied vol at that expiry."
    )
    expiry: Optional[float] = Field(
        None, description="'market': the expiry read, in years."
    )
    note: str = ""


class PricingResponse(BaseModel):
    npv: float
    greeks: Greeks
    bond_analytics: Optional[BondAnalytics] = None
    diagnostics: str
    mc_std_error: float
    risks: Optional[List[RiskEntry]] = None
    warnings: List[ModelWarning] = Field(default_factory=list)
    model_choice: Optional[ModelChoice] = Field(
        None, description="Scripted payoffs only: the model used and why."
    )
    script: Optional[str] = Field(
        None, description="Scripted library products only: the script priced."
    )
    compute_ms: Optional[float] = Field(
        None,
        description="Server-side wall time of the pricing, in milliseconds: "
        "market data, calibration and engine (`timings` splits it); excludes "
        "network, request parsing and `timings.greeks_ms`.",
    )
    timings: Optional[ComputeTimings] = Field(
        None,
        description="Where the server time went (compute_timing.py). Null "
        "outside the pricing routes.",
    )
    bumped_greeks: List[str] = Field(
        default_factory=list,
        description="The greeks the engine does not give, computed by bumping an "
        "input and revaluing (greeks_fallback.py): finite differences, with no "
        "standard error. The others are the engine's own.",
    )
    device: Literal["cpu", "gpu"] = Field(
        "cpu", description="Where the pricing actually ran."
    )
    gpus: int = Field(
        0,
        description="GPUs the Monte-Carlo shared its paths between (0 on the "
        "CPU). The result is the same bits for any count (blueprint WP 19 §7).",
    )
    second_order_method: Optional[str] = Field(
        None,
        description="Scripts priced with second_order=True: how gamma, vanna "
        "and volga were computed (the bump on the GPU's adjoint, its bumps and "
        "pairing). Null when they were not.",
    )
    precision: Optional[PrecisionOutcome] = Field(
        None, description="Set when priced to a precision (the request's `precision`)."
    )
    discretisation_bias_std_error: Optional[float] = Field(
        None,
        description="Standard error of discretisation_bias, when it comes "
        "from halving the steps: the spread of the two runs' replicate-wise "
        "differences (common random numbers), or the two standard errors "
        "combined without replicates. A bias below about twice this is not "
        "resolved: the estimate cannot tell it from zero.",
    )
    discretisation_bias: Optional[float] = Field(
        None,
        description="Estimated discretisation (time-step) bias of npv, in its "
        "units: what mc_std_error leaves out (blueprint WP 26 §9.4). From the "
        "multilevel run's bias test, or from halving a plain run's steps; 0 "
        "for a model simulated exactly between the product's dates; null when "
        "not estimated (a model that cannot refine its grid, AAD greeks, a "
        "non-Monte-Carlo engine).",
    )


class ConvergencePoint(BaseModel):
    """One pricing of a convergence study (convergence.py)."""

    paths: int
    seed: int = Field(..., description="Each point has its own: they are independent.")
    npv: float
    std_error: float = Field(..., description="The engine's own standard error.")
    compute_ms: float


class ConvergenceResponse(BaseModel):
    """The engine's standard error measured at N/32 ... N/2 paths
    (POST <pricing route>/convergence). The point at N paths is the pricing
    route's own answer to the same request: the caller adds it, rather than
    the server pricing it a second time."""

    n_paths: int = Field(
        ...,
        description="The request's path count, defaults applied: that of the "
        "pricing that is the study's last point.",
    )
    points: List[ConvergencePoint] = Field(
        ..., description="Smallest path count first; empty below 2 000 paths."
    )


class ComputeDevicesResponse(BaseModel):
    """What the server can price on (GET /price/devices)."""

    cpu: str = Field(
        ...,
        description="CPU model. The Monte-Carlo engines price on one thread of it.",
    )
    gpus: List[str] = Field(
        default_factory=list,
        description="Names of the usable CUDA devices; empty when the server "
        "has none or the native module was built without the CUDA backend.",
    )
    gpu_compiled: bool = Field(
        ..., description="Whether the native module has the CUDA backend."
    )


class MarketHistoryPoint(BaseModel):
    date: date
    close: float


class MarketHistoryResponse(BaseModel):
    ticker: str
    currency: str = Field(
        "USD", description="ISO currency of the closes (never converted)."
    )
    points: List[MarketHistoryPoint]


class RatePoint(BaseModel):
    tenor: float = Field(description="Years.")
    rate: float = Field(description="Decimal (0.0397 = 3.97 %).")


class QuotedRatePoint(BaseModel):
    tenor: float
    label: str
    series_id: str
    rate: float


class GovernmentCurveResponse(BaseModel):
    name: str
    source: str
    source_url: str
    quote: Literal["par_semiannual", "zero_continuous"]
    as_of: date
    quoted: List[QuotedRatePoint]
    zero: Optional[List[RatePoint]] = Field(
        description="Continuously compounded zero rates, sampled between the first "
        "and last pillar; None when not derivable (see no_derivation)."
    )
    forward: Optional[List[RatePoint]] = Field(
        description="Continuously compounded forwards over forward_period_years, by start tenor."
    )
    forward_period_years: float
    no_derivation: Optional[str] = None


class BenchmarkRate(BaseModel):
    series_id: str
    label: str
    kind: Literal["overnight", "policy", "compounded", "interbank_monthly"]
    backward_looking: bool
    rate: Optional[float]
    as_of: Optional[date]


class MethodologySection(BaseModel):
    title: str
    paragraphs: List[str]


class RatesOverviewResponse(BaseModel):
    currency: Literal["USD", "EUR", "GBP", "CHF", "JPY"]
    unit: Literal["decimal"] = "decimal"
    government: Optional[GovernmentCurveResponse]
    government_unavailable: Optional[str] = None
    benchmarks: List[BenchmarkRate]
    headline_series: str
    methodology: List[MethodologySection]
    warnings: List[str]


FxCurrency = Literal["USD", "EUR", "GBP", "JPY", "CHF"]


class FxForwardPoint(BaseModel):
    label: str
    tenor: float
    forward: float = Field(description="QUOTE units per BASE.")
    points: float = Field(description="Forward − spot, QUOTE units.")


class FxOverviewResponse(BaseModel):
    base: FxCurrency
    quote: FxCurrency
    spot: float = Field(description="QUOTE units per BASE (ECB reference rates).")
    spot_date: date
    forwards: Optional[List[FxForwardPoint]] = None
    forwards_unavailable: Optional[str] = None
    curve_dates: dict[str, date] = {}
    realised_vol: dict[str, Optional[float]] = Field(
        description="Annualised volatility of daily log returns, by window (1Y, 3Y, 5Y)."
    )
    methodology: List["MethodologySection"]
    warnings: List[str]


class FxHistoryPoint(BaseModel):
    date: date
    rate: float


class FxHistoryResponse(BaseModel):
    base: FxCurrency
    quote: FxCurrency
    points: List[FxHistoryPoint]


class FxCorrelationResponse(BaseModel):
    ticker: str
    base: FxCurrency
    quote: FxCurrency
    window: Literal["1Y", "3Y", "5Y"]
    frequency: Literal["weekly", "daily"]
    correlation: float
    n: int = Field(description="Number of common returns.")
    ci_low: float = Field(description="95 % confidence interval (Fisher transform).")
    ci_high: float
    asset_vol: float
    fx_vol: float
    start: date
    end: date
    asset_last: float = Field(description="Asset close on the last common date.")
    fx_last: float = Field(description="FX rate on the last common date.")


class RateHistoryPoint(BaseModel):
    date: date
    rate: float


class RatesHistoryResponse(BaseModel):
    currency: str
    series_id: str
    points: List[RateHistoryPoint]


# ---------------------------------------------------------------------------
# Autocall
# ---------------------------------------------------------------------------


class AutocallRequest(BaseModel):
    spot: float = Field(..., gt=0.0)
    rate: float
    dividend: float = 0.0
    vol: float = Field(..., gt=0.0)
    observation_dates: List[float] = Field(..., min_length=1)
    autocall_barrier: float = Field(1.0, gt=0.0)
    coupon_barrier: float = Field(0.8, gt=0.0)
    put_barrier: float = Field(0.6, gt=0.0)
    coupon_rate: float = Field(0.05, ge=0.0)
    notional: float = 1000.0
    memory_coupon: bool = True
    ki_continuous: bool = False
    n_paths: int = 200000
    seed: int = 1


# ---------------------------------------------------------------------------
# Mountain / Himalaya
# ---------------------------------------------------------------------------


class MountainRequest(BaseModel):
    spots: List[float] = Field(..., min_length=2)
    vols: List[float] = Field(..., min_length=2)
    dividends: List[float] = Field(default_factory=list)
    correlations: List[List[float]] = Field(default_factory=list)
    observation_dates: List[float] = Field(..., min_length=1)
    strike: float = 0.0
    is_call: bool = True
    rate: float = 0.05
    notional: float = 100.0
    n_paths: int = 200000
    seed: int = 1


# ---------------------------------------------------------------------------
# Variance Swap
# ---------------------------------------------------------------------------


class VarianceSwapRequest(BaseModel):
    spot: float = Field(..., gt=0.0)
    rate: float
    dividend: float = 0.0
    vol: float = Field(..., gt=0.0)
    maturity: float = Field(..., gt=0.0)
    strike_var: float = Field(..., ge=0.0, description="Annualised variance strike")
    notional: float = 100.0
    observation_dates: List[float] = Field(default_factory=list)
    engine: EngineType = EngineType.analytic
    n_paths: int = 200000
    seed: int = 1


# ---------------------------------------------------------------------------
# Volatility Swap
# ---------------------------------------------------------------------------


class VolatilitySwapRequest(BaseModel):
    spot: float = Field(..., gt=0.0)
    rate: float
    dividend: float = 0.0
    vol: float = Field(..., gt=0.0)
    maturity: float = Field(..., gt=0.0)
    strike_vol: float = Field(..., ge=0.0, description="Annualised vol strike")
    notional: float = 100.0
    observation_dates: List[float] = Field(default_factory=list)
    n_paths: int = 200000
    seed: int = 1


# ---------------------------------------------------------------------------
# Dispersion Swap
# ---------------------------------------------------------------------------


class DispersionSwapRequest(BaseModel):
    spots: List[float] = Field(..., min_length=2)
    vols: List[float] = Field(..., min_length=2)
    dividends: List[float] = Field(default_factory=list)
    weights: List[float] = Field(default_factory=list)
    pairwise_correlation: float = Field(0.0, ge=-0.999, le=0.999)
    maturity: float = Field(..., gt=0.0)
    strike_spread: float = 0.0
    rate: float = 0.05
    notional: float = 100.0
    observation_dates: List[float] = Field(default_factory=list)
    n_paths: int = 200000
    seed: int = 1


# ---------------------------------------------------------------------------
# FX Forward
# ---------------------------------------------------------------------------


class FXForwardRequest(BaseModel):
    spot: float = Field(..., gt=0.0, description="Spot FX rate (domestic per foreign)")
    rate_domestic: float
    rate_foreign: float
    vol: float = Field(0.1, ge=0.0)
    strike: float = Field(..., gt=0.0)
    maturity: float = Field(..., gt=0.0)
    notional: float = 1.0


# ---------------------------------------------------------------------------
# FX Option
# ---------------------------------------------------------------------------


class FXOptionRequest(BaseModel):
    spot: float = Field(..., gt=0.0)
    rate_domestic: float
    rate_foreign: float
    vol: float = Field(..., gt=0.0)
    strike: float = Field(..., gt=0.0)
    maturity: float = Field(..., gt=0.0)
    is_call: bool = True
    notional: float = 1.0


class QuantoRequest(BaseModel):
    """A European option on a foreign-currency asset, paid in the domestic
    currency at a conversion rate fixed in advance (Reiner 1992). Rates and
    vols are decimals; `correlation` is corr(asset, FX) with the FX rate
    quoted domestic per foreign (a EUR asset paid in USD: EUR/USD) — the
    /market/fx/correlation endpoint estimates it."""

    spot: float = Field(
        ..., gt=0.0, description="Asset, in its own (foreign) currency."
    )
    strike: float = Field(..., gt=0.0, description="In the foreign currency.")
    maturity: float = Field(..., gt=0.0)
    rate_domestic: float = Field(description="Payment currency's rate.")
    rate_foreign: float = Field(description="Asset currency's rate.")
    dividend: float = 0.0
    vol: float = Field(..., gt=0.0, description="Asset volatility σ_S.")
    fx_vol: float = Field(..., gt=0.0, description="FX volatility σ_X.")
    correlation: float = Field(..., ge=-1.0, le=1.0)
    fx_rate: float = Field(
        1.0, gt=0.0, description="Fixed conversion rate, domestic per foreign."
    )
    is_call: bool = True
    engine: Literal["analytic", "mc"] = "analytic"
    n_paths: int = Field(200_000, ge=1_000, le=5_000_000)
    seed: int = 1


# ---------------------------------------------------------------------------
# Commodity Forward
# ---------------------------------------------------------------------------


class CommodityForwardRequest(BaseModel):
    spot: float = Field(..., gt=0.0)
    rate: float
    storage_cost: float = 0.0
    convenience_yield: float = 0.0
    vol: float = Field(0.2, ge=0.0)
    strike: float = Field(..., gt=0.0)
    maturity: float = Field(..., gt=0.0)
    notional: float = 1.0


# ---------------------------------------------------------------------------
# Commodity Option
# ---------------------------------------------------------------------------


class CommodityOptionRequest(BaseModel):
    spot: float = Field(..., gt=0.0)
    rate: float
    storage_cost: float = 0.0
    convenience_yield: float = 0.0
    vol: float = Field(..., gt=0.0)
    strike: float = Field(..., gt=0.0)
    maturity: float = Field(..., gt=0.0)
    is_call: bool = True
    notional: float = 1.0


# ---------------------------------------------------------------------------
# Rainbow (worst-of / best-of)
# ---------------------------------------------------------------------------


class RainbowKind(str, Enum):
    worst_of = "worst-of"
    best_of = "best-of"


class RainbowRequest(BaseModel):
    spots: List[float] = Field(..., min_length=2)
    vols: List[float] = Field(..., min_length=2)
    dividends: List[float] = Field(default_factory=list)
    pairwise_correlation: float = Field(0.0, ge=-0.999, le=0.999)
    maturity: float = Field(..., gt=0.0)
    strike: float = Field(1.0, description="Performance strike (1.0 = ATM)")
    is_call: bool = True
    rate: float = 0.05
    notional: float = 100.0
    rainbow_kind: RainbowKind = RainbowKind.worst_of
    n_paths: int = 200000
    seed: int = 1


class LocalVolResponse(BaseModel):
    """Pricing result from the Dupire local-vol MC engine."""

    ticker: str
    spot: float
    npv: float
    mc_std_error: float
    delta: Optional[float] = None
    gamma: Optional[float] = None
    theta: Optional[float] = None
    rho: Optional[float] = None
    vega_parallel: Optional[float] = None
    n_clean_quotes: int
    cleaning_summary: str
    diagnostics: str


class CleanedIVSurfaceResponse(BaseModel):
    """Cleaned & smoothed IV surface grid (bicubic spline evaluated on a regular mesh)."""

    ticker: str
    spot: float
    strikes: List[float]
    maturities: List[float]
    values: List[List[Optional[float]]]
    n_clean_quotes: int
    cleaning_summary: str


class DeltaBucketRow(BaseModel):
    """One maturity's desk-style delta-bucketed smile: 10/25-delta put, ATM,
    25/10-delta call, plus the risk reversals and butterflies a desk
    actually quotes skew and convexity as."""

    ttm: float
    tenor_label: str
    vol_10p: Optional[float] = None
    vol_25p: Optional[float] = None
    vol_atm: Optional[float] = None
    vol_25c: Optional[float] = None
    vol_10c: Optional[float] = None
    rr25: Optional[float] = None
    bf25: Optional[float] = None
    rr10: Optional[float] = None
    bf10: Optional[float] = None


class DeltaSurfaceResponse(BaseModel):
    ticker: str
    spot: float
    rows: List[DeltaBucketRow]
    n_clean_quotes: int
    cleaning_summary: str


class LocalVolSurfaceResponse(BaseModel):
    """Dupire local-volatility surface grid."""

    ticker: str
    spot: float
    strikes: List[float]
    maturities: List[float]
    values: List[List[Optional[float]]]
    n_clean_quotes: int
    cleaning_summary: str


class SimulationModel(str, Enum):
    black_scholes = "black_scholes"
    sabr = "sabr"
    local_vol = "local_vol"
    heston = "heston"
    slv = "slv"


class BSPathRequest(BaseModel):
    spot: float = Field(gt=0)
    rate: float = 0.05
    dividend: float = 0.0
    vol: float = Field(gt=0)
    ttm: float = Field(gt=0)
    n_steps: int = Field(100, ge=2, le=1000)
    n_paths: int = Field(30, ge=1, le=500)
    seed: int = 1


class SABRPathRequest(BaseModel):
    forward: float = Field(gt=0)
    alpha: float = Field(gt=0)
    beta: float = Field(0.5, ge=0, le=1)
    rho: float = Field(ge=-1, le=1)
    nu: float = Field(gt=0)
    ttm: float = Field(gt=0)
    n_steps: int = Field(100, ge=2, le=1000)
    n_paths: int = Field(30, ge=1, le=500)
    seed: int = 1


class SimulationPathsResponse(BaseModel):
    model: SimulationModel
    time_grid: List[float]
    paths: List[List[float]]


class SimulationCalibrateRequest(BaseModel):
    ticker: str
    model: SimulationModel
    ttm: float = Field(
        gt=0,
        description="Target maturity in years -- the nearest calibrated slice is used",
    )
    rate: float = 0.05
    beta: float = Field(
        0.5,
        ge=0,
        le=1,
        description="SABR beta, fixed rather than calibrated (see SABRParams)",
    )


class SimulationCalibrateResponse(BaseModel):
    ticker: str
    model: SimulationModel
    spot: float
    dividend: float
    forward: float
    ttm: float
    slice_ttm: Optional[float] = Field(
        None,
        description="Black-Scholes and SABR: the calibrated SVI slice's own maturity, closest to the requested ttm",
    )
    vol: Optional[float] = None
    alpha: Optional[float] = None
    beta: Optional[float] = None
    rho: Optional[float] = None
    nu: Optional[float] = None
    rmse: Optional[float] = None
    converged: Optional[bool] = None
    n_clean_quotes: Optional[int] = None
    cleaning_summary: Optional[str] = None
    snapshot: Optional[date] = Field(
        None, description="Local vol, Heston, SLV: the stored option-chain snapshot."
    )
    surface: Optional[str] = Field(
        None, description="Local vol, SLV: the calibrated surface, in words."
    )
    heston: Optional[HestonFit] = None
    leverage: Optional[LeverageFit] = None


class HestonParams(BaseModel):
    v0: float = Field(..., ge=0)
    kappa: float = Field(..., gt=0)
    theta: float = Field(..., gt=0)
    xi: float = Field(..., gt=0)
    rho: float = Field(..., ge=-1, le=1)


class ModelPathRequest(BaseModel):
    """Paths of local vol, Heston or SLV. Local vol and SLV need a `ticker`:
    the surface (and the SLV leverage) come from its stored option chain.
    Heston takes `heston` with `spot`, `dividend` typed, or a `ticker` whose
    surface it is calibrated to."""

    model: Literal["local_vol", "heston", "slv"]
    ticker: Optional[str] = Field(None, min_length=1)
    spot: Optional[float] = Field(None, gt=0)
    dividend: Optional[float] = Field(
        None,
        description="Typed Heston only (default 0); a ticker's comes from the database.",
    )
    heston: Optional[HestonParams] = None
    rate: float = 0.05
    ttm: float = Field(gt=0, le=10)
    n_steps: int = Field(100, ge=2, le=1000)
    n_paths: int = Field(30, ge=1, le=500)
    seed: int = 1

    @model_validator(mode="after")
    def _inputs(self) -> "ModelPathRequest":
        typed = self.heston is not None and self.spot is not None
        if self.model == "heston" and not (typed or self.ticker):
            raise ValueError("heston needs its parameters and a spot, or a ticker")
        if self.model != "heston" and not self.ticker:
            raise ValueError(f"{self.model} needs a ticker (its surface is calibrated)")
        return self


class SABRParamsIn(BaseModel):
    alpha: float = Field(..., gt=0)
    beta: float = Field(0.5, ge=0, le=1)
    rho: float = Field(..., ge=-1, le=1)
    nu: float = Field(..., ge=0)


class DistributionRequest(BaseModel):
    """The law of a model's paths (engines/mc/path_distribution.hpp, WP 29).

    Black-Scholes takes `vol`, SABR `sabr` (with `spot` the forward), Heston
    `heston`, each with `spot` and `dividend` typed; or a `ticker`. Local vol
    and SLV always need the `ticker`: their surface is its stored chain.
    With a ticker, the stored SVI surface is returned beside the model
    (`market`), at the same dates and moneyness."""

    model: SimulationModel
    ticker: Optional[str] = Field(None, min_length=1)
    spot: Optional[float] = Field(None, gt=0)
    dividend: float = 0.0
    vol: Optional[float] = Field(None, gt=0)
    sabr: Optional[SABRParamsIn] = None
    heston: Optional[HestonParams] = None
    rate: float = 0.05
    ttm: float = Field(1.0, gt=0, le=5)
    n_steps: int = Field(60, ge=4, le=250)
    n_paths: int = Field(10_000, ge=1_000, le=20_000)
    n_display: int = Field(30, ge=0, le=100)
    seed: int = Field(1, ge=1)
    smile_times: Optional[List[float]] = Field(
        None,
        max_length=4,
        description="Maturities of the vanilla smiles, in years (the nearest "
        "grid date is used). Default: a quarter, half and all of ttm.",
    )
    moneyness: List[float] = Field(
        default_factory=lambda: [round(0.7 + 0.05 * i, 2) for i in range(13)],
        min_length=1,
        max_length=25,
        description="Strikes as K / F(0, t).",
    )
    forward_start: Optional[float] = Field(
        None,
        gt=0,
        description="Start of the forward smile, in years (to ttm). Default: ttm / 2.",
    )
    barrier: float = Field(
        1.2, gt=1, description="Up-and-out barrier, as a multiple of the spot."
    )
    barrier_strike: float = Field(1.0, gt=0)

    @model_validator(mode="after")
    def _inputs(self) -> "DistributionRequest":
        if self.ticker is None:
            if self.model in (SimulationModel.local_vol, SimulationModel.slv):
                raise ValueError(
                    f"{self.model.value} needs a ticker (its surface is calibrated)"
                )
            needed = {
                SimulationModel.black_scholes: self.vol,
                SimulationModel.sabr: self.sabr,
                SimulationModel.heston: self.heston,
            }[self.model]
            if needed is None or self.spot is None:
                raise ValueError(
                    f"{self.model.value} needs its parameters and a spot, or a ticker"
                )
        if self.barrier <= self.barrier_strike:
            raise ValueError("the barrier must be above the strike")
        if any(m <= 0 for m in self.moneyness):
            raise ValueError("moneyness must be > 0")
        return self


class SmileOut(BaseModel):
    time: float = Field(
        ..., description="Option maturity in years (T - t1 for the forward smile)."
    )
    start: float = Field(0.0, description="Forward smile: t1. Spot smiles: 0.")
    forward: float
    moneyness: List[float]
    strikes: List[float]
    prices: List[float] = Field(
        ..., description="Out of the money, discounted to today."
    )
    price_stderr: List[float]
    model_iv: List[Optional[float]] = Field(
        ..., description="Black-76; null outside the no-arbitrage bounds."
    )
    iv_stderr: List[Optional[float]]
    market_iv: Optional[List[Optional[float]]] = Field(
        None,
        description="The stored SVI surface at the same moneyness; null where not quoted.",
    )


class MartingaleOut(BaseModel):
    mean: List[float] = Field(..., description="E[S_t / N_t], Monte Carlo.")
    stderr: List[float]
    exact: List[float] = Field(..., description="S0 e^{-q t}.")


class HistogramOut(BaseModel):
    lo: List[float] = Field(..., description="Per date, the 0.5% quantile of S_t.")
    hi: List[float] = Field(..., description="Per date, the 99.5% quantile of S_t.")
    counts: List[List[int]] = Field(
        ..., description="[date][bin], equal bins on [lo, hi]."
    )
    market_density: Optional[List[Optional[List[Optional[float]]]]] = Field(
        None,
        description="[date][bin]: the stored surface's risk-neutral density of S_t "
        "at each bin centre (Breeden-Litzenberger); null outside the quoted dates.",
    )


class BarrierOut(BaseModel):
    barrier: float
    strike: float
    price: float
    stderr: float
    vanilla_price: float
    vanilla_stderr: float


class DistributionMarket(BaseModel):
    ticker: str
    snapshot: date
    spot: float
    dividend: float
    first_slice: float = Field(
        ..., description="Maturity of the first surface slice, in years."
    )
    last_slice: float
    arbitrage_dates: List[float] = Field(
        default_factory=list,
        description="Dates whose stored smile is not a density (butterfly or "
        "call-spread arbitrage): no market density is drawn there.",
    )


class DistributionResponse(BaseModel):
    model: SimulationModel
    spot: float
    rate: float
    dividend: float
    n_paths: int
    time_grid: List[float]
    paths: List[List[float]]
    vols: Optional[List[List[Optional[float]]]] = Field(
        None, description="Instantaneous vol of each display path (null at t = 0)."
    )
    quantile_levels: List[float]
    spot_quantiles: List[List[float]] = Field(..., description="[level][date]")
    vol_quantiles: Optional[List[List[Optional[float]]]] = None
    martingale: MartingaleOut
    atm_vol: List[Optional[float]] = Field(
        ..., description="ATM-forward implied vol per date."
    )
    atm_vol_stderr: List[Optional[float]]
    histogram: HistogramOut
    smiles: List[SmileOut]
    forward_smile: Optional[SmileOut] = None
    barrier: Optional[BarrierOut] = None
    market: Optional[DistributionMarket] = None


class RiskProfileRequest(BaseModel):
    """A library product whose risk profile to compute (risk_profile.py)."""

    product: str = Field(..., min_length=1)
    terms: Dict[str, float] = Field(default_factory=dict)


class RiskProfileRow(BaseModel):
    factor: str
    bump: str = Field(..., description="The bump applied to the parameter.")
    change: float = Field(..., description="Price change for the bump.")
    std_error: float = Field(
        ..., description="Paired Monte-Carlo standard error of the change."
    )
    position: Literal["long", "short", "not significant", "negligible"] = Field(
        ...,
        description="The holder's position: long if the price rises with the "
        "parameter. 'not significant' within two standard errors, "
        "'negligible' below 0.01 % of the price.",
    )


class RiskProfileResponse(BaseModel):
    product: str
    price: float
    price_std_error: float
    reference: Dict[str, float] = Field(
        ..., description="The reference market the profile is computed on."
    )
    rows: List[RiskProfileRow]
    method: str


class QuoteVegaRow(BaseModel):
    ttm: float = Field(..., description="Maturity of the quote, years.")
    strike: float
    log_moneyness: float = Field(..., description="ln(K / F_T).")
    implied_vol: float = Field(..., description="The quoted implied vol.")
    vega: float = Field(
        ..., description="Price change for +1 vol point on this quote alone."
    )
    std_error: float = Field(..., description="Monte-Carlo standard error of the vega.")


class MaturityVega(BaseModel):
    ttm: float
    vega: float = Field(..., description="Sum of the quote vegas of the maturity.")
    std_error: float
    n_quotes: int


class MarketVegaResponse(BaseModel):
    """dV / d(each quoted implied vol) of a library product under the local
    vol of the stored chain: the Dupire superbucket (lot 17h)."""

    product: str
    ticker: str
    snapshot: date
    npv: float
    mc_std_error: float
    total_vega: float = Field(
        ..., description="Sum over the quotes: a parallel +1 vol point on the chain."
    )
    total_std_error: float
    maturities: List[MaturityVega]
    quotes: List[QuoteVegaRow]
    method: str
    device: Literal["cpu", "gpu"] = Field(
        "cpu",
        description="Where the adjoint Monte-Carlo ran: the CPU tape, or the "
        "GPU's per-path adjoint (blueprint WP 19 §6).",
    )
