import type {
	BacktestConfiguration,
	BacktestStrategyRequest,
	BacktestValidationRequest,
} from "@/shared/api";

/** What the page's URL holds: a backtest is a link. */
export type BacktestView = {
	tickers: string[];
	start: string;
	/** Empty: the last day the database has. */
	end: string;
	method: BacktestConfiguration["method"];
	/** Trading days of the estimation window. */
	lookback: number;
	/** 0: on the first day of each month; otherwise every n trading days. */
	rebalanceDays: number;
	/** Cap on each weight, in percent (minimum variance, maximum Sharpe). */
	maxWeightPct: number;
	/** Cost of a trade, in basis points of the amount traded. */
	costBps: number;
	timing: "next_close" | "next_open";
	replications: number;
	blockLength: number;
	seed: number;
	lags: number;
	factors: "ff3" | "ff5";
};

/** Seven funds across equities, bonds and gold: none of them was chosen for
 * having survived, which a list of today's large companies would be. */
export const DEFAULT_VIEW: BacktestView = {
	tickers: ["SPY", "EFA", "EEM", "TLT", "IEF", "LQD", "GLD"],
	start: "2008-01-02",
	end: "",
	method: "minimum_variance",
	lookback: 252,
	rebalanceDays: 0,
	maxWeightPct: 40,
	costBps: 5,
	timing: "next_close",
	replications: 2000,
	blockLength: 5,
	seed: 42,
	lags: 9,
	factors: "ff3",
};

export const INITIAL_CAPITAL = 1_000_000;

export const METHODS: { value: BacktestView["method"]; label: string }[] = [
	{ value: "minimum_variance", label: "Minimum variance" },
	{ value: "maximum_sharpe", label: "Maximum Sharpe" },
	{ value: "risk_parity", label: "Risk parity" },
	{ value: "equal_weight", label: "Equal weight" },
];

/** The two methods that take a bound on the weights; the engine refuses one
 * on the others. */
export const isBounded = (method: BacktestView["method"]) =>
	method === "minimum_variance" || method === "maximum_sharpe";

const b64 = {
	encode: (text: string) =>
		btoa(unescape(encodeURIComponent(text)))
			.replace(/\+/g, "-")
			.replace(/\//g, "_")
			.replace(/=+$/, ""),
	decode: (text: string) =>
		decodeURIComponent(
			escape(atob(text.replace(/-/g, "+").replace(/_/g, "/"))),
		),
};

export const encodeView = (view: BacktestView) =>
	b64.encode(JSON.stringify(view));

export function decodeView(text?: string): BacktestView {
	if (!text) return DEFAULT_VIEW;
	try {
		const parsed = JSON.parse(b64.decode(text)) as Partial<BacktestView>;
		const view = { ...DEFAULT_VIEW, ...parsed };
		return Array.isArray(view.tickers) ? view : DEFAULT_VIEW;
	} catch {
		return DEFAULT_VIEW;
	}
}

/** What the form refuses before the engine does. */
export function problems(view: BacktestView, last: string): string[] {
	const out: string[] = [];
	if (view.tickers.length < 2) out.push("Pick at least two assets.");
	if (view.start >= (view.end || last))
		out.push("The start date must be before the end date.");
	if (isBounded(view.method) && view.maxWeightPct * view.tickers.length < 100)
		out.push(
			`A cap of ${view.maxWeightPct}% on ${view.tickers.length} assets cannot add up to 100%.`,
		);
	return out;
}

function configuration(
	view: BacktestView,
	method = view.method,
	lookback = view.lookback,
): BacktestConfiguration {
	return {
		method,
		lookback,
		rebalance: view.rebalanceDays === 0 ? "monthly" : view.rebalanceDays,
		max_weight: isBounded(method) ? view.maxWeightPct / 100 : 1,
	};
}

function window(view: BacktestView, last: string) {
	return {
		tickers: view.tickers,
		start: view.start,
		end: view.end || last,
		initial_capital: INITIAL_CAPITAL,
		proportional_cost_bps: view.costBps,
		timing: view.timing,
	};
}

export function strategyRequest(
	view: BacktestView,
	last: string,
): BacktestStrategyRequest {
	return {
		...window(view, last),
		...configuration(view),
		bootstrap_replications: view.replications,
		mean_block_length: view.blockLength,
		seed: view.seed,
		newey_west_lags: view.lags,
		factor_dataset: view.factors,
	};
}

/** The grid of the validation panel: every method that is always defined,
 * on three estimation windows. Maximum Sharpe is left out: it has no
 * portfolio on a day when no asset has a positive expected excess return,
 * and one failing configuration fails the grid. */
export const GRID_METHODS = [
	"minimum_variance",
	"risk_parity",
	"equal_weight",
] as const;
export const GRID_LOOKBACKS = [63, 126, 252] as const;

/** Parameters of the four tests, declared on the page. */
export const VALIDATION = {
	train: 504,
	test: 126,
	groups: 8,
	test_groups: 2,
	purge: 5,
	embargo: 5,
	blocks: 10,
} as const;

export function validationRequest(
	view: BacktestView,
	last: string,
): BacktestValidationRequest {
	return {
		...window(view, last),
		grid: GRID_METHODS.flatMap((method) =>
			GRID_LOOKBACKS.map((lookback) => configuration(view, method, lookback)),
		),
		...VALIDATION,
	};
}
