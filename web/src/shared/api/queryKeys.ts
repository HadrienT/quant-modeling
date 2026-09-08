/**
 * Hierarchical cache-key factory — blueprint WP 02 §3, dependencies.md §3.3.
 * Never write a literal key array in a component. Invalidations reference these.
 *
 * Concrete case (WP 09): pricing a portfolio invalidates its positions, its
 * risk summary and its VaR — but NOT the market surfaces.
 */

export const queryKeys = {
	health: () => ["health"] as const,

	session: () => ["session"] as const,

	market: {
		all: () => ["market"] as const,
		tickers: () => ["market", "tickers"] as const,
		history: (ticker: string, range: string) =>
			["market", "history", ticker, range] as const,
		ivSurface: (ticker: string, surface: string) =>
			["market", "iv-surface", ticker, surface] as const,
		cleanedIvSurface: (ticker: string) =>
			["market", "iv-surface-cleaned", ticker] as const,
		localVolSurface: (ticker: string) =>
			["market", "local-vol-surface", ticker] as const,
		ratesCurve: (curve: string, kind: string, fixedPeriodYears: number) =>
			["market", "rates", curve, kind, fixedPeriodYears] as const,
	},

	pricing: {
		all: () => ["pricing"] as const,
		option: (endpoint: string, params: unknown) =>
			["pricing", endpoint, params] as const,
	},

	portfolio: {
		all: () => ["portfolio"] as const,
		list: () => ["portfolio", "list"] as const,
		detail: (id: string) => ["portfolio", "detail", id] as const,
		risk: (id: string) => ["portfolio", "risk", id] as const,
		var: (id: string, confidence: number, horizon: number) =>
			["portfolio", "var", id, confidence, horizon] as const,
		stress: (id: string) => ["portfolio", "stress", id] as const,
	},

	backtest: {
		all: () => ["backtest"] as const,
		run: (params: unknown) => ["backtest", "run", params] as const,
	},
} as const;
