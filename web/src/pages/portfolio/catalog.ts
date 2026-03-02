/* Product catalog for the Add Position form */

export type ProductCategoryKey =
	| "vanilla"
	| "exotic"
	| "fixed-income"
	| "structured"
	| "volatility"
	| "fx"
	| "commodity"
	| "rainbow";

export type ProductTypeKey =
	| "european-call"
	| "european-put"
	| "american-call"
	| "american-put"
	| "asian"
	| "barrier"
	| "digital"
	| "lookback"
	| "basket"
	| "zero-coupon-bond"
	| "fixed-rate-bond"
	| "future"
	| "autocall"
	| "mountain"
	| "variance-swap"
	| "volatility-swap"
	| "dispersion-swap"
	| "fx-forward"
	| "fx-option"
	| "commodity-forward"
	| "commodity-option"
	| "rainbow";

export type ParamDef = {
	key: string;
	label: string;
	type: "number" | "boolean" | "select" | "number[]";
	default?: unknown;
	options?: { label: string; value: string }[];
	min?: number;
	step?: number;
};

export type ProductDef = {
	type: ProductTypeKey;
	category: ProductCategoryKey;
	label: string;
	params: ParamDef[];
};

/* ── Shared parameter groups ─────────────────────────────────────────────── */

const EQUITY_BASE: ParamDef[] = [
	{ key: "spot", label: "Spot", type: "number", default: 100, min: 0, step: 0.01 },
	{ key: "strike", label: "Strike", type: "number", default: 100, min: 0, step: 0.01 },
	{ key: "maturity", label: "Maturity (y)", type: "number", default: 1, min: 0.01, step: 0.01 },
	{ key: "rate", label: "Rate", type: "number", default: 0.05, step: 0.001 },
	{ key: "dividend", label: "Dividend", type: "number", default: 0, step: 0.001 },
	{ key: "vol", label: "Vol", type: "number", default: 0.2, min: 0.001, step: 0.01 },
];

const MC_PARAMS: ParamDef[] = [
	{ key: "n_paths", label: "MC paths", type: "number", default: 200000, min: 1000 },
	{ key: "seed", label: "Seed", type: "number", default: 1, min: 0 },
];

/* ── Full product catalog ────────────────────────────────────────────────── */

export const PRODUCT_CATALOG: ProductDef[] = [
	// ── Vanilla ──────────────────────────────────────────
	{
		type: "european-call", category: "vanilla", label: "European Call",
		params: [
			...EQUITY_BASE,
			{
				key: "engine", label: "Engine", type: "select", default: "analytic",
				options: [
					{ label: "Analytic", value: "analytic" },
					{ label: "MC", value: "mc" },
					{ label: "Binomial", value: "binomial" },
					{ label: "PDE", value: "pde" },
				],
			},
		],
	},
	{
		type: "european-put", category: "vanilla", label: "European Put",
		params: [
			...EQUITY_BASE,
			{
				key: "engine", label: "Engine", type: "select", default: "analytic",
				options: [
					{ label: "Analytic", value: "analytic" },
					{ label: "MC", value: "mc" },
					{ label: "Binomial", value: "binomial" },
					{ label: "PDE", value: "pde" },
				],
			},
		],
	},
	{
		type: "american-call", category: "vanilla", label: "American Call",
		params: [
			...EQUITY_BASE,
			{
				key: "engine", label: "Engine", type: "select", default: "binomial",
				options: [
					{ label: "Binomial", value: "binomial" },
					{ label: "Trinomial", value: "trinomial" },
				],
			},
			{ key: "tree_steps", label: "Tree steps", type: "number", default: 100, min: 10 },
		],
	},
	{
		type: "american-put", category: "vanilla", label: "American Put",
		params: [
			...EQUITY_BASE,
			{
				key: "engine", label: "Engine", type: "select", default: "binomial",
				options: [
					{ label: "Binomial", value: "binomial" },
					{ label: "Trinomial", value: "trinomial" },
				],
			},
			{ key: "tree_steps", label: "Tree steps", type: "number", default: 100, min: 10 },
		],
	},
	// ── Exotic ───────────────────────────────────────────
	{
		type: "asian", category: "exotic", label: "Asian",
		params: [
			...EQUITY_BASE,
			{
				key: "is_call", label: "Call/Put", type: "select", default: "true",
				options: [{ label: "Call", value: "true" }, { label: "Put", value: "false" }],
			},
			{
				key: "average_type", label: "Average", type: "select", default: "arithmetic",
				options: [{ label: "Arithmetic", value: "arithmetic" }, { label: "Geometric", value: "geometric" }],
			},
			{
				key: "engine", label: "Engine", type: "select", default: "analytic",
				options: [{ label: "Analytic", value: "analytic" }, { label: "MC", value: "mc" }],
			},
			...MC_PARAMS,
		],
	},
	{
		type: "barrier", category: "exotic", label: "Barrier",
		params: [
			...EQUITY_BASE,
			{
				key: "is_call", label: "Call/Put", type: "select", default: "true",
				options: [{ label: "Call", value: "true" }, { label: "Put", value: "false" }],
			},
			{ key: "barrier_level", label: "Barrier", type: "number", default: 120, min: 0 },
			{
				key: "barrier_kind", label: "Barrier kind", type: "select", default: "up-and-out",
				options: [
					{ label: "Up & In", value: "up-and-in" },
					{ label: "Up & Out", value: "up-and-out" },
					{ label: "Down & In", value: "down-and-in" },
					{ label: "Down & Out", value: "down-and-out" },
				],
			},
			{ key: "rebate", label: "Rebate", type: "number", default: 0, min: 0 },
			...MC_PARAMS,
		],
	},
	{
		type: "digital", category: "exotic", label: "Digital",
		params: [
			...EQUITY_BASE,
			{
				key: "is_call", label: "Call/Put", type: "select", default: "true",
				options: [{ label: "Call", value: "true" }, { label: "Put", value: "false" }],
			},
			{
				key: "payoff_type", label: "Payoff", type: "select", default: "cash-or-nothing",
				options: [{ label: "Cash-or-Nothing", value: "cash-or-nothing" }, { label: "Asset-or-Nothing", value: "asset-or-nothing" }],
			},
			{ key: "cash_amount", label: "Cash amount", type: "number", default: 1, min: 0 },
		],
	},
	{
		type: "lookback", category: "exotic", label: "Lookback",
		params: [
			...EQUITY_BASE,
			{
				key: "is_call", label: "Call/Put", type: "select", default: "true",
				options: [{ label: "Call", value: "true" }, { label: "Put", value: "false" }],
			},
			{
				key: "style", label: "Style", type: "select", default: "fixed-strike",
				options: [{ label: "Fixed Strike", value: "fixed-strike" }, { label: "Floating Strike", value: "floating-strike" }],
			},
			{
				key: "extremum", label: "Extremum", type: "select", default: "maximum",
				options: [{ label: "Maximum", value: "maximum" }, { label: "Minimum", value: "minimum" }],
			},
			...MC_PARAMS,
		],
	},
	{
		type: "basket", category: "exotic", label: "Basket",
		params: [
			{ key: "spots", label: "Spots (comma-sep)", type: "number[]", default: [100, 100] },
			{ key: "vols", label: "Vols (comma-sep)", type: "number[]", default: [0.2, 0.2] },
			{ key: "strike", label: "Strike", type: "number", default: 100, min: 0 },
			{ key: "maturity", label: "Maturity (y)", type: "number", default: 1, min: 0.01 },
			{ key: "rate", label: "Rate", type: "number", default: 0.05 },
			{
				key: "is_call", label: "Call/Put", type: "select", default: "true",
				options: [{ label: "Call", value: "true" }, { label: "Put", value: "false" }],
			},
			{ key: "pairwise_correlation", label: "Correlation", type: "number", default: 0.5, min: -0.999, step: 0.01 },
			...MC_PARAMS,
		],
	},
	// ── Fixed income ─────────────────────────────────────
	{
		type: "zero-coupon-bond", category: "fixed-income", label: "Zero-Coupon Bond",
		params: [
			{ key: "maturity", label: "Maturity (y)", type: "number", default: 2, min: 0.01 },
			{ key: "rate", label: "Rate", type: "number", default: 0.03 },
			{ key: "notional", label: "Notional", type: "number", default: 1000, min: 0 },
		],
	},
	{
		type: "fixed-rate-bond", category: "fixed-income", label: "Fixed-Rate Bond",
		params: [
			{ key: "maturity", label: "Maturity (y)", type: "number", default: 3, min: 0.01 },
			{ key: "rate", label: "Rate", type: "number", default: 0.03 },
			{ key: "coupon_rate", label: "Coupon rate", type: "number", default: 0.045, min: 0 },
			{ key: "coupon_frequency", label: "Cpn freq", type: "number", default: 2, min: 1 },
			{ key: "notional", label: "Notional", type: "number", default: 1000, min: 0 },
		],
	},
	{
		type: "future", category: "fixed-income", label: "Equity Future",
		params: [
			{ key: "spot", label: "Spot", type: "number", default: 100, min: 0 },
			{ key: "strike", label: "Strike", type: "number", default: 100, min: 0 },
			{ key: "maturity", label: "Maturity (y)", type: "number", default: 0.25, min: 0.01 },
			{ key: "rate", label: "Rate", type: "number", default: 0.05 },
			{ key: "dividend", label: "Dividend", type: "number", default: 0 },
			{ key: "notional", label: "Notional", type: "number", default: 1, min: 0 },
		],
	},
	// ── Structured ───────────────────────────────────────
	{
		type: "autocall", category: "structured", label: "Autocall",
		params: [
			{ key: "spot", label: "Spot", type: "number", default: 100, min: 0 },
			{ key: "rate", label: "Rate", type: "number", default: 0.05 },
			{ key: "dividend", label: "Dividend", type: "number", default: 0 },
			{ key: "vol", label: "Vol", type: "number", default: 0.2, min: 0.001 },
			{ key: "observation_dates", label: "Obs dates (y)", type: "number[]", default: [0.25, 0.5, 0.75, 1.0] },
			{ key: "autocall_barrier", label: "Autocall barrier", type: "number", default: 1.0 },
			{ key: "coupon_barrier", label: "Coupon barrier", type: "number", default: 0.8 },
			{ key: "put_barrier", label: "Put barrier", type: "number", default: 0.6 },
			{ key: "coupon_rate", label: "Coupon rate", type: "number", default: 0.05, min: 0 },
			{ key: "notional", label: "Notional", type: "number", default: 1000 },
			...MC_PARAMS,
		],
	},
	{
		type: "mountain", category: "structured", label: "Mountain (Himalaya)",
		params: [
			{ key: "spots", label: "Spots (comma-sep)", type: "number[]", default: [100, 100, 100] },
			{ key: "vols", label: "Vols (comma-sep)", type: "number[]", default: [0.2, 0.2, 0.2] },
			{ key: "observation_dates", label: "Obs dates (y)", type: "number[]", default: [0.5, 1.0, 1.5] },
			{ key: "strike", label: "Strike", type: "number", default: 0 },
			{ key: "rate", label: "Rate", type: "number", default: 0.05 },
			{ key: "notional", label: "Notional", type: "number", default: 100 },
			...MC_PARAMS,
		],
	},
	// ── Volatility ───────────────────────────────────────
	{
		type: "variance-swap", category: "volatility", label: "Variance Swap",
		params: [
			{ key: "spot", label: "Spot", type: "number", default: 100, min: 0 },
			{ key: "rate", label: "Rate", type: "number", default: 0.05 },
			{ key: "dividend", label: "Dividend", type: "number", default: 0 },
			{ key: "vol", label: "Vol", type: "number", default: 0.2, min: 0.001 },
			{ key: "maturity", label: "Maturity (y)", type: "number", default: 1, min: 0.01 },
			{ key: "strike_var", label: "Var strike", type: "number", default: 0.04, min: 0 },
			{ key: "notional", label: "Notional", type: "number", default: 100 },
			{
				key: "engine", label: "Engine", type: "select", default: "analytic",
				options: [{ label: "Analytic", value: "analytic" }, { label: "MC", value: "mc" }],
			},
			...MC_PARAMS,
		],
	},
	{
		type: "volatility-swap", category: "volatility", label: "Volatility Swap",
		params: [
			{ key: "spot", label: "Spot", type: "number", default: 100, min: 0 },
			{ key: "rate", label: "Rate", type: "number", default: 0.05 },
			{ key: "dividend", label: "Dividend", type: "number", default: 0 },
			{ key: "vol", label: "Vol", type: "number", default: 0.2, min: 0.001 },
			{ key: "maturity", label: "Maturity (y)", type: "number", default: 1, min: 0.01 },
			{ key: "strike_vol", label: "Vol strike", type: "number", default: 0.2, min: 0 },
			{ key: "notional", label: "Notional", type: "number", default: 100 },
			...MC_PARAMS,
		],
	},
	{
		type: "dispersion-swap", category: "volatility", label: "Dispersion Swap",
		params: [
			{ key: "spots", label: "Spots (comma-sep)", type: "number[]", default: [100, 100] },
			{ key: "vols", label: "Vols (comma-sep)", type: "number[]", default: [0.2, 0.25] },
			{ key: "maturity", label: "Maturity (y)", type: "number", default: 1, min: 0.01 },
			{ key: "strike_spread", label: "Strike spread", type: "number", default: 0 },
			{ key: "rate", label: "Rate", type: "number", default: 0.05 },
			{ key: "notional", label: "Notional", type: "number", default: 100 },
			{ key: "pairwise_correlation", label: "Correlation", type: "number", default: 0.5, min: -0.999, step: 0.01 },
			...MC_PARAMS,
		],
	},
	// ── FX ────────────────────────────────────────────────
	{
		type: "fx-forward", category: "fx", label: "FX Forward",
		params: [
			{ key: "spot", label: "Spot FX", type: "number", default: 1.1, min: 0, step: 0.0001 },
			{ key: "rate_domestic", label: "Rate domestic", type: "number", default: 0.05 },
			{ key: "rate_foreign", label: "Rate foreign", type: "number", default: 0.03 },
			{ key: "vol", label: "Vol", type: "number", default: 0.1, min: 0 },
			{ key: "strike", label: "Strike", type: "number", default: 1.1, min: 0 },
			{ key: "maturity", label: "Maturity (y)", type: "number", default: 1, min: 0.01 },
			{ key: "notional", label: "Notional", type: "number", default: 1000000 },
		],
	},
	{
		type: "fx-option", category: "fx", label: "FX Option",
		params: [
			{ key: "spot", label: "Spot FX", type: "number", default: 1.1, min: 0, step: 0.0001 },
			{ key: "rate_domestic", label: "Rate domestic", type: "number", default: 0.05 },
			{ key: "rate_foreign", label: "Rate foreign", type: "number", default: 0.03 },
			{ key: "vol", label: "Vol", type: "number", default: 0.1, min: 0.001 },
			{ key: "strike", label: "Strike", type: "number", default: 1.1, min: 0 },
			{ key: "maturity", label: "Maturity (y)", type: "number", default: 1, min: 0.01 },
			{
				key: "is_call", label: "Call/Put", type: "select", default: "true",
				options: [{ label: "Call", value: "true" }, { label: "Put", value: "false" }],
			},
			{ key: "notional", label: "Notional", type: "number", default: 1000000 },
		],
	},
	// ── Commodity ─────────────────────────────────────────
	{
		type: "commodity-forward", category: "commodity", label: "Commodity Forward",
		params: [
			{ key: "spot", label: "Spot", type: "number", default: 80, min: 0 },
			{ key: "rate", label: "Rate", type: "number", default: 0.05 },
			{ key: "storage_cost", label: "Storage cost", type: "number", default: 0.02 },
			{ key: "convenience_yield", label: "Conv yield", type: "number", default: 0.01 },
			{ key: "vol", label: "Vol", type: "number", default: 0.2, min: 0 },
			{ key: "strike", label: "Strike", type: "number", default: 80, min: 0 },
			{ key: "maturity", label: "Maturity (y)", type: "number", default: 1, min: 0.01 },
			{ key: "notional", label: "Notional", type: "number", default: 100 },
		],
	},
	{
		type: "commodity-option", category: "commodity", label: "Commodity Option",
		params: [
			{ key: "spot", label: "Spot", type: "number", default: 80, min: 0 },
			{ key: "rate", label: "Rate", type: "number", default: 0.05 },
			{ key: "storage_cost", label: "Storage cost", type: "number", default: 0.02 },
			{ key: "convenience_yield", label: "Conv yield", type: "number", default: 0.01 },
			{ key: "vol", label: "Vol", type: "number", default: 0.3, min: 0.001 },
			{ key: "strike", label: "Strike", type: "number", default: 80, min: 0 },
			{ key: "maturity", label: "Maturity (y)", type: "number", default: 1, min: 0.01 },
			{
				key: "is_call", label: "Call/Put", type: "select", default: "true",
				options: [{ label: "Call", value: "true" }, { label: "Put", value: "false" }],
			},
			{ key: "notional", label: "Notional", type: "number", default: 100 },
		],
	},
	// ── Rainbow ───────────────────────────────────────────
	{
		type: "rainbow", category: "rainbow", label: "Rainbow (Worst/Best-of)",
		params: [
			{ key: "spots", label: "Spots (comma-sep)", type: "number[]", default: [100, 100] },
			{ key: "vols", label: "Vols (comma-sep)", type: "number[]", default: [0.2, 0.25] },
			{ key: "maturity", label: "Maturity (y)", type: "number", default: 1, min: 0.01 },
			{ key: "strike", label: "Strike", type: "number", default: 1.0 },
			{ key: "rate", label: "Rate", type: "number", default: 0.05 },
			{
				key: "is_call", label: "Call/Put", type: "select", default: "true",
				options: [{ label: "Call", value: "true" }, { label: "Put", value: "false" }],
			},
			{
				key: "rainbow_kind", label: "Kind", type: "select", default: "worst-of",
				options: [{ label: "Worst-of", value: "worst-of" }, { label: "Best-of", value: "best-of" }],
			},
			{ key: "pairwise_correlation", label: "Correlation", type: "number", default: 0.5, min: -0.999, step: 0.01 },
			{ key: "notional", label: "Notional", type: "number", default: 100 },
			...MC_PARAMS,
		],
	},
];

export const CATEGORIES: { key: ProductCategoryKey; label: string }[] = [
	{ key: "vanilla", label: "Vanilla" },
	{ key: "exotic", label: "Exotic" },
	{ key: "fixed-income", label: "Fixed Income" },
	{ key: "structured", label: "Structured" },
	{ key: "volatility", label: "Volatility" },
	{ key: "fx", label: "FX" },
	{ key: "commodity", label: "Commodity" },
	{ key: "rainbow", label: "Rainbow" },
];

export function catalogFor(cat: ProductCategoryKey): ProductDef[] {
	return PRODUCT_CATALOG.filter((p) => p.category === cat);
}

export function findProduct(type: ProductTypeKey): ProductDef | undefined {
	return PRODUCT_CATALOG.find((p) => p.type === type);
}
