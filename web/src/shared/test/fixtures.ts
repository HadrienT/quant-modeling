/**
 * Realistic fixtures — blueprint WP 02 §4. Not `{ npv: 42 }`: a good fixture is
 * one that would have caught a bug. Shared by MSW (tests, Storybook, offline dev).
 */

export const TICKERS = [
	"AAPL",
	"MSFT",
	"NVDA",
	"SPY",
	"TSLA",
	"AMZN",
	"GOOGL",
	"META",
	"JPM",
	"XOM",
];

/** ~250 trading days of a gently trending price with realistic noise. */
export function priceHistory(ticker: string, days = 252) {
	const points: { date: string; close: number }[] = [];
	let price = 180;
	const start = new Date("2025-01-02").getTime();
	let seed = ticker.split("").reduce((a, c) => a + c.charCodeAt(0), 0);
	const rng = () => {
		seed = (seed * 1664525 + 1013904223) % 4294967296;
		return seed / 4294967296;
	};
	for (let i = 0; i < days; i++) {
		price *= 1 + (rng() - 0.48) * 0.02;
		points.push({
			date: new Date(start + i * 86_400_000).toISOString().slice(0, 10),
			close: Math.round(price * 100) / 100,
		});
	}
	return { ticker, points };
}

/** IV surface with genuine holes (null = strike without a quote). */
export function ivSurface(ticker: string, surface = "mid") {
	const strikes = [140, 150, 160, 170, 180, 190, 200, 210, 220, 240];
	const maturities = [0.08, 0.17, 0.25, 0.5, 1, 2];
	const values = maturities.map((t) =>
		strikes.map((k) => {
			// Wings of the short expiries frequently have no quote.
			if (t <= 0.17 && (k <= 150 || k >= 220)) return null;
			const moneyness = Math.log(k / 185);
			const smile = 0.04 * moneyness * moneyness - 0.02 * moneyness;
			return Math.round((0.22 + smile + 0.01 * Math.sqrt(t)) * 10000) / 10000;
		}),
	);
	return { ticker, surface, strikes, maturities, values };
}

export function cleanedIvSurface(ticker: string) {
	const base = ivSurface(ticker);
	return {
		...base,
		spot: 185.3,
		values: base.values.map((row) => row.map((v) => (v == null ? 0.223 : v))),
		n_clean_quotes: 47,
		cleaning_summary:
			"63 raw quotes → 47 kept. Rejected: 9 wide spread, 5 stale, 2 crossed markets.",
	};
}

export function localVolSurface(ticker: string) {
	const base = cleanedIvSurface(ticker);
	return {
		...base,
		values: base.values.map((row, i) =>
			row.map((v) => Math.round((v + 0.01 * (i - 2)) * 10000) / 10000),
		),
		cleaning_summary: base.cleaning_summary + " Dupire calibrated.",
	};
}

export function ratesCurve(curve: string, kind: string) {
	const tenors = [0.08, 0.25, 0.5, 1, 2, 3, 5, 7, 10, 20, 30];
	const zero = tenors.map((x) => ({
		x,
		y:
			Math.round(
				(0.042 + 0.006 * Math.log(1 + x) - (kind === "forward" ? 0.002 : 0)) *
					10000,
			) / 10000,
	}));
	return { curve, zero };
}

export function pricingResponse(overrides?: Record<string, unknown>) {
	return {
		npv: 12.4187,
		greeks: {
			delta: 0.5421,
			gamma: 0.0187,
			vega: 0.2013,
			theta: -0.0421,
			rho: 0.1123,
			delta_std_error: 0.0009,
			vega_std_error: 0.0004,
		},
		bond_analytics: null,
		diagnostics: "engine=mc paths=128000 antithetic=on bridge=on",
		mc_std_error: 0.0231,
		...overrides,
	};
}

let pfSeq = 1;
export function portfolio(name = "Desk book") {
	const products: [string, string, string][] = [
		["european-call", "vanilla", "AAPL Dec 190 C"],
		["european-put", "vanilla", "AAPL Dec 170 P"],
		["barrier", "exotic", "MSFT up-and-out 480"],
		["autocall", "structured", "NVDA phoenix 6m"],
		["asian", "exotic", "SPY arithmetic Asian"],
		["digital", "exotic", "TSLA cash-or-nothing"],
		["fixed-rate-bond", "fixed-income", "5y 4% bond"],
		["future", "fixed-income", "SPY future"],
		["variance-swap", "volatility", "SPX var swap"],
		["basket", "exotic", "FANG basket call"],
	];
	return {
		id: `pf-${pfSeq++}`,
		name,
		owner: "",
		created_at: "2026-01-04T09:00:00Z",
		updated_at: "2026-02-01T14:30:00Z",
		positions: products.map(([product_type, category, label], i) => ({
			id: `pos-${i + 1}`,
			label,
			product_type,
			category,
			direction: i % 3 === 0 ? "short" : "long",
			quantity: (i + 1) * 10,
			entry_price: 8 + i,
			parameters: {
				spot: 185,
				strike: 190,
				maturity: 0.5,
				vol: 0.22,
				rate: 0.04,
			},
			result: {
				npv: (9 + i) * (i % 3 === 0 ? -1 : 1),
				unit_price: 9 + i * 0.5,
				greeks: {
					delta: 0.4 - i * 0.03,
					gamma: 0.02,
					vega: 0.18,
					theta: -0.03,
					rho: 0.1,
				},
				priced_at: "2026-02-01T14:30:00Z",
				engine: i % 2 ? "mc" : "analytic",
				diagnostics: "",
				mc_std_error: i % 2 ? 0.03 : 0,
			},
		})),
	};
}

export function backtestResponse() {
	const start = new Date("2015-01-02").getTime();
	const n = 120;
	const portfolio_values = Array.from({ length: n }, (_, i) => ({
		date: new Date(start + i * 30 * 86_400_000).toISOString().slice(0, 10),
		value: 100_000 * (1 + i * 0.012 + Math.sin(i / 6) * 0.04),
	}));
	const sp500_values = portfolio_values.map((p, i) => ({
		date: p.date,
		value: 100_000 * (1 + i * 0.009 + Math.sin(i / 5) * 0.03),
	}));
	return {
		opt_start: "2015-01-02",
		opt_end: "2020-01-02",
		allocation: [
			{
				ticker: "AAPL",
				weight: 0.34,
				start_price: 27.3,
				end_price: 189.1,
				return_pct: 592.7,
			},
			{
				ticker: "MSFT",
				weight: 0.28,
				start_price: 46.7,
				end_price: 421.3,
				return_pct: 802.1,
			},
			{
				ticker: "XOM",
				weight: 0.22,
				start_price: 92.8,
				end_price: 104.2,
				return_pct: 12.3,
			},
			{
				ticker: "JPM",
				weight: 0.16,
				start_price: 62.5,
				end_price: 171.4,
				return_pct: 174.2,
			},
		],
		portfolio_values,
		sp500_values,
		metrics: {
			ath: 245_000,
			atl: 96_000,
			total_return_pct: 143.2,
			annualized_return_pct: 9.3,
			max_drawdown: -0.184,
			sharpe_ratio: 0.82,
			optimal_sharpe: 1.44,
			alpha: null,
			beta: null,
		},
		warnings: [
			"XOM has only 8y of history over the 10y window — allocation may be unstable.",
			"alpha/beta not computed: benchmark series has gaps.",
		],
	};
}
