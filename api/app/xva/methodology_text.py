"""What the page says about the computation: methodology sections and wrong-way scenarios."""

from __future__ import annotations

import math
from typing import TYPE_CHECKING, List, Tuple

if TYPE_CHECKING:  # pragma: no cover — xva_equity imports the portfolio modules
    pass
from ..schemas import MethodologySection
from .demos import PORTFOLIOS

_SECTIONS: List[Tuple[str, List[str]]] = [
    (
        "What is computed",
        [
            "If a counterparty defaults while a trade is worth something to the bank, "
            "the bank loses that value less what it recovers. The credit valuation "
            "adjustment (CVA) is the price of that risk today; the debit valuation "
            "adjustment (DVA) is the same thing seen by the counterparty about the "
            "bank. Both come from one object: the distribution of the future value "
            "of the netting set, date by date.",
            "Sign convention, Gregory's (4th edition): an adjustment is negative when "
            "it is a cost to the bank. Adjusted value = risk-free value + CVA + DVA "
            "+ FVA. CVA is negative or zero, DVA positive or zero.",
        ],
    ),
    (
        "Exposure",
        [
            "V(t) is the value of the netting set to the bank at a future date, after "
            "the payment of that date. Expected exposure EE(t) is the average of "
            "max(V(t), 0); expected negative exposure ENE(t) the average of "
            "min(V(t), 0), a negative number. Potential future exposure PFE(t) is a "
            "high quantile of V(t). EPE is the time average of EE; the Effective EPE "
            "(EEPE) of the Basel framework averages, over the first year, an EE that "
            "is not allowed to decrease.",
            "The values come from a simulation: thousands of paths of interest rates "
            "on a grid of dates that includes every payment date, and each trade "
            "repriced on each path at each date. Close-out netting is applied path by "
            "path: values add up before the positive part is taken.",
        ],
    ),
    (
        "Interest-rate scenarios",
        [
            "Rates follow the one-factor Hull-White model, dr = (θ(t) − a r)dt + σ dW, "
            "fitted exactly to today's swap curve. In this model the price of every "
            "zero-coupon bond at a future date is a closed formula of one state "
            "variable, so swaps and European swaptions are repriced exactly on each "
            "path, without approximation.",
            "The two parameters are calibrated to the swaptions actually traded: the "
            "mean reversion a and the volatility σ that best reproduce the normal "
            "vols implied by traded premiums (see the Rates page). The swap curve is "
            "the median rate of the swaps traded on the latest day. These are "
            "medians of trades published by DTCC, not dealer quotes.",
        ],
    ),
    (
        "CVA and DVA",
        [
            "CVA = −LGD × Σ EE*(t_i) × S_bank(t_{i−1}) × PD_counterparty(t_{i−1}, t_i), "
            "where EE* is the discounted expected exposure, LGD = 1 − recovery, PD "
            "the probability that the counterparty defaults in the interval and "
            "S_bank the probability that the bank has not defaulted before: only the "
            "first default matters. DVA is the mirror image, on the negative "
            "exposure and the bank's own default. The unilateral CVA ignores the "
            "bank's default.",
            "Each figure carries its Monte-Carlo standard error, computed from the "
            "dispersion of the loss path by path. Rule of thumb shown next to it: "
            "CVA ≈ −credit spread × EPE × maturity.",
            "The model assumes that default is independent of the level of rates (no "
            "wrong-way risk) and that the two defaults are independent of each other.",
        ],
    ),
    (
        "Bermudans: value by regression",
        [
            "A swap and a European swaption have an exact value on every path at "
            "every date under the model. A Bermudan does not: what it is worth "
            "depends on when it will be exercised. Its value is estimated by "
            "American Monte-Carlo (Longstaff & Schwartz, 2001): on a separate, "
            "independent set of pilot paths, the cash flows it goes on to pay are "
            "regressed on the level of rates at each date, which gives its value "
            "as a function of rates. The number of pilot paths is shown with the "
            "result.",
            "Each path carries an exercise state. On an exercise date the holder "
            "enters the swap when it is worth more than keeping the option; from "
            "then on the trade is that swap, valued exactly. The rule is an "
            "estimate of the best one, so the simulated holder is slightly "
            "sub-optimal: the value shown for today is the lattice price, and the "
            "simulated exposure starts just below it. On swaps and European "
            "swaptions, where the exact value is known, the same regression "
            "reproduces the expected exposure within 1 to 2 % of its peak and the "
            "99 % quantile within 2 to 4 %, the more pilot paths the closer.",
        ],
    ),
    (
        "Trades written as scripts",
        [
            "A trade can be given as a script in the payoff language of the "
            "Scripting page, on interest rates: dated events that read the curve "
            "of their date with df(DATE), keep what they need in variables and "
            "pay with `pays`. The engine does not know what the trade is. It "
            "replays the script on every scenario, which gives its cash flows, "
            "and estimates its value at each date by regressing the cash flows "
            "still to come on the level of rates and on the script's own "
            "variables, as for a Bermudan. A swap written this way has the "
            "exposure and the CVA of the native swap, within the accuracy of the "
            "regression.",
            "This is the idea of Andreasen and Savine: the cash flows are the one "
            "description of a trade, and every calculation — value, exposure, "
            "capital — is run on them. So the capital of a scripted trade is the "
            "internal models method's, which reads the simulated exposure; the "
            "standardised approach would need a supervisory description the "
            "script does not carry. Its value today is a Monte-Carlo price, and "
            "ISDA SIMM, which needs sensitivities scenario by scenario, gives way "
            "to the regression model of the margin.",
        ],
    ),
    (
        "Wrong-way risk",
        [
            "Every figure above assumes that the counterparty's default has "
            "nothing to do with what it owes. Wrong-way risk is the case where it "
            "defaults more often precisely when it owes more. It is modelled as "
            "Hull & White (2012) do: the hazard rate depends on the value of the "
            "netting set, λ(t) = exp(a(t) + b × V(t)), with a(t) solved at each "
            "date so that the survival probability averaged over the scenarios is "
            "still the market's. The parameter moves default probability between "
            "scenarios; it does not create any.",
            "The parameter is entered per reference notional of 10 M: a value of "
            "10 multiplies the hazard by e^0.5 ≈ 1.65 in a scenario where the "
            "netting set is worth 5 % of the notional more. It has no market "
            "quote: it is a scenario, and the figure to read is the ratio of the "
            "CVA with it to the independent CVA, shown side by side. Under a "
            "collateral agreement the effect nearly disappears: the exposure is "
            "then the move of the value over ten days, which is no larger where "
            "the value is high. Funding, margin and capital adjustments keep the "
            "market survival curve.",
        ],
    ),
    (
        "Initial margin and MVA",
        [
            "Under the margin rules for non-cleared derivatives each party also "
            "posts initial margin, held apart, sized to cover 99 % of the move of "
            "the netting set's value over the margin period of risk. Pricing it "
            "needs that margin in every scenario at every date. It is projected by "
            "regression (Anfuso, Aziz, Giltinan & Loukopoulos, 2017): on the "
            "simulated paths, the squared move of the value over the next ten days "
            "is regressed on the value, which gives its conditional standard "
            "deviation; the margin is 2.33 times it, the 99 % quantile of a normal "
            "move. This is a model of the margin, not the industry's rule (ISDA "
            "SIMM works from sensitivities): if you give today's actual margin, the "
            "whole profile is scaled to start from it.",
            "When every trade is a swap or a European swaption, the margin is "
            "instead ISDA SIMM itself (methodology version 2.8+2512, interest rate "
            "risk class), computed in every scenario at every date from the "
            "sensitivities of that scenario: the PV01 at each of the twelve "
            "vertices of the curve, weighted by ISDA's risk weights and aggregated "
            "with its correlations, plus the vega and curvature of the options. The "
            "sensitivities come from the closed forms, not from a bump: each cash "
            "flow's sensitivity to its own zero rate, shared between the two "
            "vertices around it; for an option, its vega at the normal volatility "
            "its price implies. (SIMM asks for sensitivities to market rates; zero "
            "rates are used here.) SIMM is calibrated on a period of stress and "
            "comes out about one and a half times the regression model's margin.",
            "With that margin the exposure is what is left beyond it, on about one "
            "path in a hundred: the CVA almost disappears. What replaces it is the "
            "cost of funding the margin posted, MVA = −Σ E[D × IM](t) × survival of "
            "both parties × funding spread × Δt. Segregated margin funds nothing, so "
            "the funding adjustments stay those of the variation margin.",
        ],
    ),
    (
        "ColVA",
        [
            "A collateral agreement pays a rate on the cash it holds. When that "
            "rate is the overnight rate the trades are discounted at, nothing is "
            "gained or lost: this is why collateralised trades are discounted on "
            "the OIS curve (Piterbarg, 2010). When it differs, ColVA = −Σ E[D × "
            "collateral held](t) × survival of both parties × (rate paid − discount "
            "rate) × Δt: a cost when the bank holds collateral on which it pays "
            "more.",
        ],
    ),
    (
        "Capital and KVA",
        [
            "Two regulatory charges are projected. By default the exposure at default is the "
            "standardised approach's (SA-CCR, Basel framework CRE52): 1.4 × "
            "(replacement cost + multiplier × add-on). At a future date the add-on "
            "comes from the trades that are left, the same in every scenario; the "
            "replacement cost and the multiplier come from the value, and the "
            "margin held, in the scenario. The default-risk capital is the IRB "
            "formula on that exposure (CRE31, with the PD floor of 0.05 % of "
            "CRE32.4, the foundation LGD of CRE32.6 and an effective maturity "
            "capped at five years, CRE32.46); the CVA capital is the reduced basic "
            "approach (BA-CVA, MAR50), with the sector and investment-grade risk "
            "weight of the counterparty and no cap on the maturity (MAR50.15).",
            "The probability of default of that formula is a historical frequency: "
            "the share of the issuers of the counterparty's rating category that "
            "defaulted within a year, averaged over the years on record (S&P's "
            "corporate ratings, as the agency reports them to ESMA). It is not the "
            "probability the credit spread implies, which the CVA uses: a spread "
            "also pays for bearing the risk, and implies several times more "
            "defaults than have been observed. A probability of default can be "
            "entered instead.",
            "The other way to the exposure at default is the internal models "
            "method (CRE53): 1.4 × the Effective EPE, the average over the coming "
            "year of the expected exposure made non-decreasing, read off the "
            "simulation itself with its collateral. Its effective maturity is a "
            "ratio of areas under the exposure profile (CRE53.20), capped at five "
            "years for the default charge and not for the CVA charge, whose "
            "supervisory discount it replaces (MAR50.15). It needs no description "
            "of the trades, so it is the method of a netting set that holds a "
            "script; on swaps it can be asked for, to compare. Three things a "
            "bank's approved model does are not done here: the exposure of a "
            "future date is the one expected today, not the one the bank would "
            "compute in each scenario; the profile comes from the pricing "
            "simulation, which the rule allows; and the stressed calibration the "
            "rule adds (CRE53.7) is not run.",
            "KVA = −Σ E[D × capital](t) × survival of both parties × cost of capital "
            "× Δt (Green, Kenyon & Dennis, 2014). An option keeps today's moneyness "
            "in its supervisory delta, and after its last exercise date it is "
            "carried as its underlying swap. Market-risk capital of the hedges and "
            "the leverage ratio are not included.",
        ],
    ),
    (
        "Credit curves",
        [
            "No free source publishes CDS spreads by name. The counterparty and the "
            "bank are given a rating, and the rating a spread: the option-adjusted "
            "spread of the ICE BofA US corporate index of that rating (FRED). The "
            "hazard rate is the constant default intensity that reprices a five-year "
            "CDS at that spread, about spread / (1 − recovery).",
            "These are bond spreads, which differ from CDS spreads by a basis and "
            "include a liquidity premium: a proxy, as banks use for counterparties "
            "without a traded CDS.",
        ],
    ),
    (
        "Collateral",
        [
            "Under a collateral agreement (CSA) the party that is owed money receives "
            "variation margin: the value above a threshold, called when the change "
            "exceeds a minimum transfer amount. What remains is the margin period of "
            "risk: if the counterparty defaults at t, the collateral in hand is the "
            "one called on the value some days earlier (ten business days by "
            "regulatory convention), and the exposure is V(t) − C(t − MPoR).",
            "Coupons paid during that period matter: where the bank pays a coupon the "
            "value jumps while the collateral still reflects the value before it, "
            "which shows as spikes in the exposure. The treatment can be changed "
            "(both parties keep paying, both stop, or only the bank keeps paying).",
            "The comparison with the uncollateralised netting set is made on the same "
            "paths.",
        ],
    ),
    (
        "Two measures: prices and risks",
        [
            "CVA is a price: its exposure is computed on risk-neutral scenarios, "
            "calibrated to option prices. PFE and credit limits are risk measures — "
            "how large the exposure can really get — and are computed on scenarios "
            "that follow the historical behaviour of rates, dr = a(θ − r)dt + σ dW, "
            "estimated on the 3-month Treasury yield since 1990. In both cases each "
            "trade is valued on each path by the market-calibrated model: only the "
            "probability of the scenarios differs.",
            "The historical volatility σ is well determined by the data. The mean "
            "reversion a and the long-run rate θ are not: they change entirely with "
            "the estimation window, and θ drives the result (a long-run rate below "
            "today's makes payer swaps lose value on average). They are shown with "
            "their standard errors and can be set by hand.",
        ],
    ),
    (
        "The share of each trade",
        [
            "Stand-alone CVA: the trade as if it were alone. Incremental CVA: the CVA "
            "of the netting set minus its CVA without the trade — the price to charge "
            "for adding it, positive when the trade reduces the risk. Marginal CVA: "
            "the Euler allocation, the trade's value on the paths where the netting "
            "set is positive; the shares add up to the total. There is no exact "
            "allocation under a CSA with a threshold or a minimum transfer amount.",
        ],
    ),
    (
        "Where the paths are valued",
        [
            "Swaps and European swaptions have a closed-form value under the model. "
            "When the server has graphics cards, their paths are valued there: one "
            "GPU thread per path draws the random numbers the CPU would have drawn "
            "and applies the same formulas. The scenarios are identical and the "
            "values agree to about fourteen digits; the last ones differ because a "
            "graphics card does not compute an exponential exactly as a processor "
            "does. The result is the same, bit for bit, on one card or on two.",
            "A Bermudan or a scripted trade is valued by regression, and SIMM on "
            "every path needs the sensitivities of each scenario: both stay on the "
            "CPU. The response says where the paths ran and why.",
        ],
    ),
    (
        "Sources",
        [
            "Gregory, The xVA Challenge: Counterparty Risk, Funding, Collateral, "
            "Capital and Initial Margin, 4th ed., Wiley, 2020.",
            "Andreasen & Savine, Modern Computational Finance: Scripting for "
            "Derivatives and xVA, Wiley, 2021.",
            "Pykhtin & Zhu, A Guide to Modelling Counterparty Credit Risk, GARP Risk "
            "Review, 2007.",
            "Andersen, Pykhtin & Sokol, Rethinking the Margin Period of Risk, Journal "
            "of Credit Risk, 2017.",
            "Basel Committee on Banking Supervision, Basel Framework, CRE53 (internal "
            "models method: Effective EE, Effective EPE).",
            "European Securities and Markets Authority, Central Repository (CEREP): "
            "default statistics reported by the credit rating agencies. The default "
            "rates of this page were drafted using material downloaded from ESMA's "
            "website; ESMA does not endorse this publication.",
        ],
    ),
]


_SENSITIVITY_SECTIONS: List[Tuple[str, List[str]]] = [
    (
        "What is computed",
        [
            "The derivative of each adjustment — CVA, DVA, the funding cost FCA and "
            "the funding benefit FBA — to every quote the market gives: each par "
            "swap rate of the curve, each swaption volatility the model was "
            "calibrated to, the credit spread of each party, the funding spreads. "
            "It is the hedge: how much the adjustment moves when a quote moves, "
            "per unit of the quote. A sensitivity to a rate of 1 000 000 means "
            "100 per basis point.",
            "Every figure comes with its Monte-Carlo error, measured as the "
            "dispersion of the sensitivity over batches of 64 scenarios.",
        ],
    ),
    (
        "Adjoint differentiation",
        [
            "Bumping each input and running the simulation again costs two runs "
            "per input. The adjoint method (Giles & Glasserman, 2006; Savine, "
            "2018) gets all of them from one run: every operation of the "
            "valuation is recorded on a tape, then the tape is read backwards, "
            "carrying the derivative of the result to each operation in turn. The "
            "cost is a small multiple of one valuation, whatever the number of "
            "inputs; the page shows the multiple measured on this run.",
            "The set-up — the curve, the model, the coefficients of every trade, "
            "the default probabilities — is recorded once. Each scenario is then "
            "recorded, read backwards and erased, so the memory is that of one "
            "scenario: the check-pointing of Savine's book.",
        ],
    ),
    (
        "From the model to the market",
        [
            "The simulation is written in zero rates, in the two parameters of "
            "Hull-White and in hazard rates; the market quotes swap rates, "
            "swaption volatilities and credit spreads. Each quote determines the "
            "model's inputs through a calibration — the bootstrap of the curve, "
            "the fit of the two parameters to the volatilities, the bootstrap of "
            "the hazard rates — and the calibration is differentiated at its "
            "solution, by the implicit function theorem, not through its "
            "iterations. A swap rate therefore moves the adjustment three ways: "
            "through the curve, through the model recalibrated on that curve, and "
            "through the hazard rates, which reprice the same credit spreads on "
            "it.",
            "Checked against the direct calculation: move a quote, rebuild the "
            "curve, calibrate again, price again on the same scenarios.",
        ],
    ),
    (
        "What a path-by-path derivative misses",
        [
            "A swaption that is exercised becomes a swap; one that is not becomes "
            "nothing. Scenario by scenario that choice does not move when the "
            "curve moves a little, so a naive derivative ignores the scenarios "
            "that change sides — more than a tenth of the sensitivity of a bought "
            "swaption to the 10-year rate. The choice is replaced by a narrow "
            "ramp (a twentieth of a standard deviation of the swap's value on "
            "each side), which recovers them and changes the CVA by a few "
            "thousandths of a percent. A minimum transfer amount makes the "
            "collateral jump in the same way; that jump is left out, and said so.",
        ],
    ),
    (
        "SA-CVA",
        [
            "The standardised approach for CVA risk (Basel framework, MAR50.42 to "
            "50.77) computes capital from sensitivities of the regulatory CVA — "
            "the unilateral one, without the bank's own default. Three risk "
            "classes matter for interest-rate trades in one currency: the delta "
            "to the risk-free yield at 1, 2, 5, 10 and 30 years; the vega, to a "
            "relative shift of every volatility; the delta to the counterparty's "
            "credit spread at 6 months, 1, 3, 5 and 10 years. Each sensitivity is "
            "multiplied by a supervisory risk weight, the weighted sensitivities "
            "of a class are aggregated with supervisory correlations, and the "
            "classes add up. The text allows the sensitivities to be computed by "
            "adjoint differentiation (MAR50.47, FAQ 1).",
            "The swap-rate sensitivities are spread over the five prescribed "
            "tenors by linear interpolation. No hedge is recognised, and the "
            "multiplier is the default of 1.",
        ],
    ),
    (
        "Not covered",
        [
            "Trades valued by regression (Bermudans, scripts), initial margin, "
            "wrong-way risk and the capital adjustments are not differentiated.",
        ],
    ),
    (
        "Sources",
        [
            "Giles & Glasserman, Smoking adjoints: fast Monte Carlo Greeks, Risk, "
            "2006.",
            "Savine, Modern Computational Finance: AAD and Parallel Simulations, "
            "Wiley, 2018.",
            "Andreasen & Savine, Modern Computational Finance: Scripting for "
            "Derivatives and xVA, Wiley, 2021 (smoothing of discontinuities).",
            "Capriotti & Giles, Fast correlation Greeks by adjoint algorithmic "
            "differentiation, Risk, 2010.",
            "Basel Committee on Banking Supervision, Basel Framework, MAR50 "
            "(minimum capital requirements for CVA risk).",
        ],
    ),
]


_TWO_FACTOR_SENSITIVITY_SECTIONS: List[Tuple[str, List[str]]] = [
    (
        "With a second currency or a share",
        [
            "The simulation then has more than one source of randomness — the dollar "
            "rate, and a euro rate with the exchange rate, or a share — and so has "
            "the adjoint pass: the same algorithm, on the transitions of the joint "
            "model. Every trade is still a formula of the scenario: a forward and a "
            "cross-currency swap are known cash flows, an equity option is Black's "
            "formula on the forward of the scenario, a euro swap is read on the euro "
            "rate and converted at the exchange rate of the scenario.",
            "The volatility of the exchange rate or of the share in the model is not "
            "quoted: it is set so that one option has its market volatility, and "
            "that calibration depends on the rate models and on the correlations. "
            "The sensitivities shown hold the market volatility fixed, so a move of "
            "a swaption volatility also counts what it does to the share's or the "
            "exchange rate's volatility in the model.",
            "Beside the swap rates, swaption volatilities and credit spreads, the "
            "second factor has its own: the price of the share or the exchange rate "
            "(the delta of the adjustment), its market volatility (the vega), the "
            "dividend yield or the euro zero rates, and the correlations. The "
            "correlations are historical estimates: their sensitivities measure "
            "what an estimation error costs. The euro curve is reported by zero rate "
            "at each of its pillars and the euro rate model by its two parameters, "
            "not yet mapped to euro swap rates and swaption volatilities.",
            "SA-CVA is not reported for such a netting set: the capital would have "
            "to add the foreign-exchange or equity risk class to the interest rate "
            "and credit spread ones, and a figure without it would understate it.",
        ],
    ),
]


def sensitivities_methodology(two_factors: bool = False) -> List[MethodologySection]:
    sections = _SENSITIVITY_SECTIONS + (
        _TWO_FACTOR_SENSITIVITY_SECTIONS if two_factors else []
    )
    return [MethodologySection(title=t, paragraphs=p) for t, p in sections]


_TWO_CURRENCY_SECTIONS: List[Tuple[str, List[str]]] = [
    (
        "Two currencies",
        [
            "Each currency has its own interest rate, a one-factor Hull-White model "
            "calibrated on that currency's swap curve and swaption trades. The "
            "exchange rate is lognormal, and the three move together through three "
            "correlations. Everything is valued in dollars: a euro trade is worth its "
            "value in euros, on the euro rate of the scenario, times the exchange "
            "rate of the scenario.",
            "What is simulated is not the spot but the forward exchange rate to the "
            "last date of the netting set, F(t, T*) = S(t) P_EUR(t, T*) / P_USD(t, T*). "
            "It has no drift under the measure the simulation runs in, and a "
            "volatility known in advance, so the three factors are jointly Gaussian "
            "and each step is drawn exactly, whatever its length. The spot is read "
            "back from the forward and the two rates. Seen from the dollar measure, "
            "the euro rate carries a drift −ρ σ_EUR σ_S, the quanto adjustment.",
            "Source: Brigo & Mercurio, Interest Rate Models — Theory and Practice, "
            "2nd ed., Springer, 2006, §14.3; Piterbarg, Smiling hybrids, Risk, 2006.",
        ],
    ),
    (
        "The volatility of the exchange rate",
        [
            "Implied FX volatilities are not published for free, but the trades are: "
            "every FX option dealt by a US person is published with its premium by "
            "DTCC's swap data repository. An option's premium says little without "
            "the forward it was struck against, and the store holds one spot fixing "
            "a day while the rate moves by half a percent in a session. A straddle "
            "— a call and a put with the same strike, traded together — carries its "
            "own forward by put-call parity, C − P = DF (F − K), and with it the "
            "volatility that reprices either leg. The term structure is the median "
            "over the at-the-money straddles of each expiry.",
            "The model's spot volatility is the one that gives the option of the "
            "calibration expiry its market volatility. The model then gives a "
            "volatility to every expiry — the forward also carries the volatility "
            "of both rates — and the table shows it next to the market's. One "
            "number cannot follow a term structure: the fit is exact at one expiry.",
            "Source: Garman & Kohlhagen, Foreign currency option values, Journal of "
            "International Money and Finance, 1983.",
        ],
    ),
    (
        "The correlations",
        [
            "Between dollar and euro rates, and of each with the exchange rate. "
            "No freely published price implies them — quanto and cross-currency "
            "options do, and are not disseminated — so they are estimated on three "
            "years of weekly changes of the five-year yields of each currency and of "
            "the ECB's reference rate, with a 95 % confidence interval. Weekly, "
            "because the series are not observed at the same hour and daily changes "
            "would bias the estimate towards zero.",
            "A cross-currency swap's exposure is that of its final exchange of "
            "notionals: Gregory, The xVA Challenge, 4th ed., Wiley, 2020, the "
            "exposure profiles of the chapter on future value and exposure.",
        ],
    ),
]


_EQUITY_SECTIONS = [
    (
        "Equity options in the netting set",
        [
            "A share is simulated with the interest rate: its price is lognormal, "
            "the rate follows Hull-White, and the two are correlated. Technically "
            "the share takes the place the second currency has in the two-currency "
            "model — its price is the exchange rate, its dividend yield a foreign "
            "rate that is known and does not move (Brigo & Mercurio, Interest Rate "
            "Models, 2nd ed., Springer, 2006, §14.3 and appendix B on equity under "
            "stochastic rates).",
            "A European option is then a closed form on every scenario: Black's "
            "formula on the forward price of that scenario, with the variance the "
            "forward still has to accumulate until the expiry, which is known "
            "today. A bought option is an asset until it expires, pays its "
            "intrinsic value then, and is nothing after; a sold one is a negative "
            "quantity. Options are settled in cash and on one share each.",
            "The share's volatility in the model is set so that the option the "
            "model is calibrated on has its market Black volatility: the implied "
            "volatility of the day's smile at its strike and expiry, or the "
            "share's realised volatility over 63 business days when no option "
            "chain is stored (a proxy, said in the warnings). There is one "
            "volatility: several options are valued without the smile between them.",
            "The correlation of the share with the interest rate is a historical "
            "estimate — weekly changes of the five-year Treasury yield against "
            "weekly log returns of the share over three years — given with its 95 % "
            "confidence interval. The dividend yield is the stored trailing yield, "
            "held constant.",
            "One share per netting set, next to dollar swaps and European "
            "swaptions. American, Asian, barrier, digital and lookback options, "
            "trades in euros and a second share are left out, and the warnings say "
            "so. As with two currencies: on the CPU, under the pricing measure, "
            "with the margin modelled by regression.",
        ],
    ),
]


def methodology(
    two_currencies: bool = False, equity: bool = False
) -> List[MethodologySection]:
    sections = (
        _SECTIONS
        + (_TWO_CURRENCY_SECTIONS if two_currencies else [])
        + (_EQUITY_SECTIONS if equity else [])
    )
    return [MethodologySection(title=t, paragraphs=p) for t, p in sections]


#: The move of the netting set's value the explanations are written for, as a
#: share of the reference notional.
_WRONG_WAY_MOVE = 0.05


def _hazard_change(b: float) -> str:
    """What `wrong_way_risk` = b does to the hazard rate when the netting set
    gains _WRONG_WAY_MOVE of the reference notional: exp(b × move)."""
    factor = math.exp(b * _WRONG_WAY_MOVE)
    if factor >= 2.0:
        return f"is multiplied by {factor:.1f}"
    if factor >= 1.0:
        return f"rises by {100.0 * (factor - 1.0):.0f} %"
    return f"falls by {100.0 * (1.0 - factor):.0f} %"


def wrong_way_scenarios() -> List[Tuple[str, str, float, str]]:
    """(id, label, wrong_way_risk, explanation): the scenarios the page offers
    instead of a number to type. The parameter has no market quote, and the
    credit curves here are rating proxies, on which it could not be estimated:
    it is a scenario, and each one says what it assumes."""
    move = f"{100.0 * _WRONG_WAY_MOVE:.0f} % of the reference notional"
    closing = (
        " The survival curve of the market is kept: default probability moves "
        "between scenarios, none is created. A scenario, not an estimate "
        "(Hull & White, 2012)."
    )
    return [
        (
            "independent",
            "Independent",
            0.0,
            "The counterparty's default has nothing to do with what it owes: the "
            "standard CVA, and the reference the other scenarios are read against.",
        ),
        (
            "wrong_way",
            "Wrong-way",
            10.0,
            "The counterparty defaults more often when it owes more: its hazard "
            f"rate {_hazard_change(10.0)} in a scenario where the netting set is "
            f"worth {move} more, and {_hazard_change(-10.0)} where it is worth "
            "that much less." + closing,
        ),
        (
            "strong_wrong_way",
            "Strong wrong-way",
            20.0,
            "The same dependence, twice as strong: the hazard rate "
            f"{_hazard_change(20.0)} where the netting set is worth {move} more."
            + closing,
        ),
        (
            "right_way",
            "Right-way",
            -10.0,
            "The counterparty defaults less often when it owes more — it is hedging "
            f"its own business with the trade: its hazard rate {_hazard_change(-10.0)} "
            f"where the netting set is worth {move} more." + closing,
        ),
    ]


def portfolios() -> List[Tuple[str, str, str]]:
    return [(pid, label, lesson) for pid, (label, lesson, _) in PORTFOLIOS.items()]


__all__ = [
    "compute",
    "compute_sensitivities",
    "market_inputs",
    "methodology",
    "portfolios",
    "wrong_way_scenarios",
    "PORTFOLIOS",
    "RATINGS",
    "XvaInputError",
    "XvaUnavailable",
]
