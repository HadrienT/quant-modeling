import type { BacktestHedgingRequest, BacktestMetric } from "@/shared/api";

/** The hedges the engine knows, in the page's order, with what each is. */
export const HEDGES = [
	{
		id: "black_scholes_fixed",
		label: "Black-Scholes, fixed vol",
		help: "Black-Scholes delta at one volatility, today's, for the whole life of the option.",
	},
	{
		id: "black_scholes_implied",
		label: "Black-Scholes, implied vol",
		help: "Black-Scholes delta at the option's implied volatility, read again at each date.",
	},
	{
		id: "skew_adjusted",
		label: "Skew-adjusted",
		help: "The Black-Scholes delta at the implied volatility plus the vega times the slope of the smile in the strike, dσ/dK: Derman's (1999) local-volatility rule of thumb.",
	},
	{
		id: "minimum_variance",
		label: "Minimum variance",
		help: "Hull and White's (2017) delta, fitted on other paths than the ones it is measured on.",
	},
	{
		id: "true_model",
		label: "The world's own delta",
		help: "The delta of the model the paths follow: the hedge a trader who knew the model would use.",
	},
] as const;

export type HedgeId = (typeof HEDGES)[number]["id"];
export const hedgeLabel = (id: string) =>
	HEDGES.find((h) => h.id === id)?.label ?? id;

export type WorldKind = "black_scholes" | "heston" | "ticker";

/** The form, as typed: percents as the page shows them. */
export type HedgingForm = {
	world: WorldKind;
	ticker: string;
	spot: string;
	ratePct: string;
	dividendPct: string;
	volPct: string;
	v0: string;
	kappa: string;
	theta: string;
	xi: string;
	rho: string;
	strikePct: string;
	maturity: string;
	isCall: boolean;
	hedges: HedgeId[];
	rebalances: string;
	paths: string;
	seed: string;
	costBps: string;
};

export const DEFAULT_FORM: HedgingForm = {
	world: "heston",
	ticker: "SPY",
	spot: "100",
	ratePct: "4",
	dividendPct: "0",
	volPct: "20",
	v0: "0.04",
	kappa: "2",
	theta: "0.04",
	xi: "0.5",
	rho: "-0.7",
	strikePct: "100",
	maturity: "0.5",
	isCall: true,
	hedges: ["black_scholes_implied", "skew_adjusted", "true_model"],
	rebalances: "50",
	paths: "5000",
	seed: "1",
	costBps: "0",
};

/** Paths at most per world (the API's bound: a Heston world prices the option
 * on every path at every date). */
export const maxPaths = (world: WorldKind) =>
	world === "black_scholes" ? 50_000 : 10_000;

export function hedgingRequest(f: HedgingForm): BacktestHedgingRequest {
	const n = Number;
	const heston = {
		v0: n(f.v0),
		kappa: n(f.kappa),
		theta: n(f.theta),
		xi: n(f.xi),
		rho: n(f.rho),
	};
	const common = {
		spot: n(f.spot),
		rate: n(f.ratePct) / 100,
		dividend_yield: n(f.dividendPct) / 100,
		volatility: n(f.volPct) / 100,
		...heston,
	};
	return {
		world:
			f.world === "ticker"
				? { ...common, model: "heston", ticker: f.ticker.trim().toUpperCase() }
				: { ...common, model: f.world, ticker: null },
		strike: n(f.strikePct) / 100,
		maturity: n(f.maturity),
		is_call: f.isCall,
		hedges: f.hedges,
		hedge_volatility: null,
		rebalances: n(f.rebalances),
		paths: n(f.paths),
		seed: n(f.seed),
		cost_rate: n(f.costBps) / 10_000,
		confidence: 0.95,
	};
}

/** "value ± half-width" of a metric's interval, in currency. */
export function withInterval(m: BacktestMetric | null | undefined, digits = 3) {
	if (!m || m.value == null) return "n/a";
	const v = m.value.toFixed(digits).replace("-", "−");
	if (m.lower == null || m.upper == null) return v;
	return `${v} ± ${((m.upper - m.lower) / 2).toFixed(digits)}`;
}

export const SOURCES = {
	dermanKamal:
		"Derman, E. & Kamal, M. (1999). When you cannot hedge continuously: the corrections of Black-Scholes. Risk 12(1).",
	derman: "Derman, E. (1999). Regimes of volatility. Risk 12(4).",
	hullWhite:
		"Hull, J. & White, A. (2017). Optimal delta hedging for options. Journal of Banking & Finance 82.",
	heston:
		"Heston, S. (1993). A closed-form solution for options with stochastic volatility. Review of Financial Studies 6(2).",
} as const;
