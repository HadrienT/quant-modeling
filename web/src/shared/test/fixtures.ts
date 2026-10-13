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
	const currency = ticker.endsWith(".PA")
		? "EUR"
		: ticker.endsWith(".L")
			? "GBP"
			: "USD";
	return { ticker, currency, points };
}

export const MARKETS = [
	{
		id: "SP500",
		name: "S&P 500",
		currency: "USD",
		members: 503,
		as_of: "2026-09-24",
		has_options: true,
		note: "Option chains for a fixed set of liquid names.",
	},
	{
		id: "CAC40",
		name: "CAC 40",
		currency: "EUR",
		members: 40,
		as_of: "2026-09-24",
		has_options: false,
		note: "No free option data: Yahoo Finance publishes option chains for US listings only.",
	},
];

export function marketTickers(market: string | null) {
	if (market === "CAC40") {
		const members = [
			{ ticker: "^FCHI", name: "CAC 40", kind: "index", currency: "EUR" },
			{ ticker: "MC.PA", name: "LVMH", kind: "equity", currency: "EUR" },
			{ ticker: "AIR.PA", name: "Airbus", kind: "equity", currency: "EUR" },
		];
		return { tickers: members.map((m) => m.ticker), members };
	}
	const members = TICKERS.map((t) => ({
		ticker: t,
		name: null,
		kind: "equity",
		currency: "USD",
	}));
	return market
		? { tickers: TICKERS, members }
		: { tickers: TICKERS, members: [] };
}

/** Strike/maturity grid with genuine holes (null = strike without a quote). */
function ivSurfaceGrid(ticker: string) {
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
	return { ticker, strikes, maturities, values };
}

/** The un-cleaned, un-fitted listed grid (GET /api/local-vol/raw-surface). */
export function rawIvSurface(ticker: string) {
	const base = ivSurfaceGrid(ticker);
	const nWithIv = base.values.flat().filter((v) => v != null).length;
	return {
		...base,
		spot: 185.3,
		n_clean_quotes: nWithIv,
		cleaning_summary: `63 raw quotes -> with a usable implied vol: ${nWithIv}. No cleaning applied.`,
	};
}

export function cleanedIvSurface(ticker: string) {
	const base = ivSurfaceGrid(ticker);
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

const METHODOLOGY = [
	{ title: "Data", paragraphs: ["Read from the project's own database."] },
	{
		title: "Derived zero and forward curves",
		paragraphs: ["Log-linear discount factors; continuous compounding."],
	},
];

export function ratesOverview(currency: string) {
	const tenors = [0.25, 0.5, 1, 2, 5, 10, 30];
	const z = (x: number) =>
		Math.round((0.03 + 0.006 * Math.log(1 + x)) * 1e6) / 1e6;
	const quoted = tenors.map((tenor) => ({
		tenor,
		label: tenor < 1 ? `${tenor * 12}M` : `${tenor}Y`,
		series_id: `S${tenor}`,
		rate: z(tenor),
	}));
	const base = {
		currency,
		unit: "decimal",
		benchmarks: [
			{
				series_id: "ON",
				label: "Overnight benchmark",
				kind: "overnight",
				backward_looking: false,
				rate: 0.0385,
				as_of: "2026-09-21",
			},
			{
				series_id: "AVG3M",
				label: "3M compounded average",
				kind: "compounded",
				backward_looking: true,
				rate: 0.0366,
				as_of: "2026-09-22",
			},
		],
		headline_series: "ON",
		methodology: [
			...METHODOLOGY,
			{ title: `${currency} specifics`, paragraphs: ["Specific notes."] },
		],
		warnings: [] as string[],
	};
	if (currency === "CHF")
		return {
			...base,
			government: null,
			government_unavailable: "No free CHF government curve.",
		};
	const derived = currency !== "GBP";
	return {
		...base,
		government: {
			name: `${currency} government curve`,
			source: "Publisher",
			source_url: "https://example.org",
			quote: "par_semiannual",
			as_of: "2026-09-18",
			quoted,
			zero: derived
				? tenors.map((tenor) => ({ tenor, rate: z(tenor) - 0.0005 }))
				: null,
			forward: derived
				? tenors
						.slice(0, -1)
						.map((tenor) => ({ tenor, rate: z(tenor) + 0.002 }))
				: null,
			forward_period_years: 0.5,
			no_derivation: derived ? null : "Only three par yields.",
		},
		government_unavailable: null,
	};
}

export function ratesHistory(currency: string, seriesId: string) {
	return {
		currency,
		series_id: seriesId,
		points: [
			{ date: "2026-09-18", rate: 0.0386 },
			{ date: "2026-09-21", rate: 0.0385 },
		],
	};
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
		compute_ms: 3.42,
		device: "cpu",
		...overrides,
	};
}

let pfSeq = 1;
/** A ledger (portfolio v2): 100 MC.PA bought, 30 sold; 2 CAC 40 calls. */
export function portfolio(name = "Desk book") {
	return {
		id: `pf-${pfSeq++}`,
		name,
		owner: "",
		version: 2,
		base_currency: "EUR" as const,
		created_at: "2026-01-04T09:00:00Z",
		updated_at: "2026-02-01T14:30:00Z",
		positions: [],
		instruments: [
			{
				id: "EQ:MC.PA",
				label: "MC.PA — LVMH",
				spec: { kind: "equity" as const, ticker: "MC.PA" },
			},
			{
				id: "cac-call",
				label: "^FCHI European option K=8200 2027-03-19",
				spec: {
					kind: "derivative" as const,
					product: "vanilla",
					params: { strike: 8200, is_call: true },
					underlying: "^FCHI",
					expiry: "2027-03-19",
					currency: "EUR" as const,
				},
			},
		],
		transactions: [
			{
				id: "t1",
				instrument_id: "EQ:MC.PA",
				trade_date: "2026-06-02",
				quantity: 100,
				price: 480,
				fees: 5,
				note: "",
				created_at: "2026-06-02T10:00:00Z",
			},
			{
				id: "t2",
				instrument_id: "EQ:MC.PA",
				trade_date: "2026-07-06",
				quantity: -30,
				price: 455,
				fees: 5,
				note: "trim",
				created_at: "2026-07-06T10:00:00Z",
			},
			{
				id: "t3",
				instrument_id: "cac-call",
				trade_date: "2026-06-10",
				quantity: 2,
				price: 250,
				fees: 0,
				note: "",
				created_at: "2026-06-10T10:00:00Z",
			},
		],
	};
}

/** Two of the API's example counterparties (portfolio_counterparties.py). */
export function counterpartyExamples() {
	const csa = {
		threshold_counterparty: 0,
		threshold_bank: 0,
		minimum_transfer_amount: 500_000,
		margin_period_of_risk_days: 10,
		cashflows: "paid" as const,
		initial_margin: false,
		initial_margin_model: "auto" as const,
		initial_margin_today: null,
		collateral_rate_spread: 0,
	};
	return [
		{
			id: "example-dealer",
			name: "Northgate Bank (dealer)",
			rating: "A" as const,
			sector: "financial" as const,
			recovery: 0.4,
			netting_sets: [
				{
					id: "example-dealer-vm",
					name: "ISDA with CSA: variation margin",
					csa,
				},
			],
		},
		{
			id: "example-corporate",
			name: "Meridian Industries (corporate)",
			rating: "BBB" as const,
			sector: "basic_materials_energy_industrials" as const,
			recovery: 0.4,
			netting_sets: [
				{
					id: "example-corporate-isda",
					name: "ISDA, uncollateralised",
					csa: null,
				},
			],
		},
	];
}

/** A 5-year payer swap struck at the par rate of its trade date. */
export function rateFxQuote() {
	return {
		spec: {
			kind: "rate_fx" as const,
			product: "swap" as const,
			currency: "USD" as const,
			notional: 10_000_000,
			payer: true,
			start: "2026-10-12",
			maturity: "2031-10-12",
			fixed_rate: 0.0365,
			foreign_rate: null,
			domestic_notional: null,
		},
		label: "Payer swap USD 10M at 3.650%, 2026-10-12 to 2031-10-12",
		value: 0,
		currency: "USD",
	};
}

export function portfolioDemos() {
	return [
		{
			portfolio: {
				...portfolio("Euro blue chips"),
				id: "demo-euro-blue-chips",
				owner: "demo",
			},
			description: "Five large euro-area names bought in January.",
		},
	];
}

export function portfolioSnapshot() {
	const input = (name: string, status: string, value: number) => ({
		name,
		status,
		value,
		as_of: "2026-09-18",
	});
	return {
		as_of: "2026-09-18",
		base_currency: "EUR",
		positions: [
			{
				instrument_id: "EQ:MC.PA",
				label: "MC.PA — LVMH",
				kind: "equity",
				currency: "EUR",
				quantity: 70,
				average_cost: 480,
				mark: 440,
				market_value: 30800,
				market_value_base: 30800,
				unrealised: -2800,
				unrealised_base: -2800,
				realised_base: -760,
				fees_base: 10,
				day_pnl_base: 140,
				fx_rate: 1,
				inputs: [input("close", "observed", 440)],
				greeks: { delta: 1 },
				note: null,
			},
			{
				instrument_id: "cac-call",
				label: "^FCHI European option K=8200 2027-03-19",
				kind: "derivative",
				currency: "EUR",
				quantity: 2,
				average_cost: 250,
				mark: 184.9,
				market_value: 369.8,
				market_value_base: 369.8,
				unrealised: -130.2,
				unrealised_base: -130.2,
				realised_base: 0,
				fees_base: 0,
				day_pnl_base: -12.4,
				fx_rate: 1,
				inputs: [
					input("spot", "observed", 8081.43),
					input("time to expiry", "observed", 0.4819),
					input("vol (63-day realised)", "proxied", 0.1185),
				],
				greeks: { delta: 0.45 },
				model: {
					model: "Black-Scholes, flat volatility",
					engine: "Black-Scholes closed form",
					why: "No option chain is stored for this underlying on this date.",
					params: [
						{ name: "strike", value: 8200, status: "contract", source: "" },
						{
							name: "volatility",
							value: 0.1185,
							status: "proxied",
							source: "^FCHI realised vol, 63 business days",
						},
					],
					std_error: null,
				},
				note: null,
			},
		],
		market_value: 31169.8,
		unrealised: -2930.2,
		realised: -760,
		fees: 10,
		total_pnl: -3690.2,
		day_pnl: 127.6,
		warnings: [],
		methodology: [
			{
				title: "Marking",
				paragraphs: ["Each position is marked with that day's market."],
			},
		],
	};
}

export function portfolioHistory() {
	return {
		base_currency: "EUR",
		warnings: [],
		points: [
			{
				date: "2026-09-16",
				market_value: 31000,
				daily_pnl: -50,
				cumulative_pnl: -3800,
			},
			{
				date: "2026-09-17",
				market_value: 31040,
				daily_pnl: -17.8,
				cumulative_pnl: -3817.8,
			},
			{
				date: "2026-09-18",
				market_value: 31169.8,
				daily_pnl: 127.6,
				cumulative_pnl: -3690.2,
			},
		],
	};
}

export function fxOverview(base: string, quote: string) {
	const withForwards = base !== "GBP" && quote !== "GBP";
	return {
		base,
		quote,
		spot: 1.1367,
		spot_date: "2026-09-24",
		forwards: withForwards
			? [
					{ label: "3M", tenor: 0.25, forward: 1.14096, points: 0.00426 },
					{ label: "1Y", tenor: 1, forward: 1.15229, points: 0.01559 },
				]
			: null,
		forwards_unavailable: withForwards
			? null
			: "Only three par yields for GBP.",
		curve_dates: withForwards ? { EUR: "2026-09-23", USD: "2026-09-22" } : {},
		realised_vol: { "1Y": 0.054, "3Y": 0.067, "5Y": 0.0764 },
		methodology: [
			{ title: "Forwards", paragraphs: ["Covered interest parity."] },
		],
		warnings: [],
	};
}

export function fxHistory(base: string, quote: string) {
	return {
		base,
		quote,
		points: [
			{ date: "2026-09-23", rate: 1.1351 },
			{ date: "2026-09-24", rate: 1.1367 },
		],
	};
}

export function fxCorrelation(ticker: string) {
	return {
		ticker,
		base: "EUR",
		quote: "USD",
		window: "3Y",
		frequency: "weekly",
		correlation: 0.308,
		n: 156,
		ci_low: 0.16,
		ci_high: 0.44,
		asset_vol: 0.162,
		fx_vol: 0.074,
		start: "2023-09-29",
		end: "2026-09-18",
		asset_last: 8087.87,
		fx_last: 1.1367,
	};
}
