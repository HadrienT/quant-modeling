const API_BASE = import.meta.env.VITE_API_BASE ?? "http://localhost:8000";
const API_KEY = import.meta.env.VITE_API_KEY;
const TOKEN_KEY = "qm_auth_token";

function getToken(): string | null {
	return localStorage.getItem(TOKEN_KEY);
}

function headers(json = true): Record<string, string> {
	const h: Record<string, string> = {};
	if (json) h["Content-Type"] = "application/json";
	if (API_KEY) h["X-API-KEY"] = API_KEY;
	const token = getToken();
	if (token) h["Authorization"] = `Bearer ${token}`;
	return h;
}

async function apiFetch<T>(url: string, init?: RequestInit): Promise<T> {
	const res = await fetch(url, init);
	if (!res.ok) {
		const text = await res.text().catch(() => "");
		throw new Error(text || `HTTP ${res.status}`);
	}
	if (res.status === 204) return undefined as unknown as T;
	return res.json();
}

export type VanillaQuoteRequest = {
	spot: number;
	strike: number;
	rate: number;
	dividend: number;
	vol: number;
	maturity: number;
	option_type: "call" | "put";
	notional: number;
};

export async function priceVanilla(payload: VanillaQuoteRequest) {
	const response = await fetch(`${API_BASE}/price/option/vanilla`, {
		method: "POST",
		headers: { "Content-Type": "application/json" },
		body: JSON.stringify(payload)
	});

	if (!response.ok) {
		throw new Error("Failed to fetch vanilla price");
	}

	return response.json();
}

export type MarketHistoryPoint = {
	date: string;
	close: number;
};

export type MarketHistoryResponse = {
	ticker: string;
	points: MarketHistoryPoint[];
};

export type TickersResponse = {
	tickers: string[];
};

export type IVSurfaceResponse = {
	ticker: string;
	surface: "mid" | "bid" | "ask";
	strikes: number[];
	maturities: number[];
	values: Array<Array<number | null>>;
};

export type RatesCurveResponse = {
	curve: "Treasury" | "SOFR" | "FedFunds";
	zero: Array<{ x: number; y: number }>;
};

export type RatesCurveType = "zero" | "forward";

export type SurfaceGridResponse = {
	ticker: string;
	spot: number;
	strikes: number[];
	maturities: number[];
	values: Array<Array<number | null>>;
	n_clean_quotes: number;
	cleaning_summary: string;
};

export async function getMarketHistory(ticker: string, range: string): Promise<MarketHistoryResponse> {
	const headers: Record<string, string> = { "Content-Type": "application/json" };
	if (API_KEY) {
		headers["X-API-KEY"] = API_KEY;
	}
	const response = await fetch(`${API_BASE}/market/prices/history?ticker=${encodeURIComponent(ticker)}&range=${range}`,
		{
			method: "GET",
			headers
		}
	);

	if (!response.ok) {
		throw new Error("Failed to fetch market history");
	}

	return response.json();
}

export async function getTickers(): Promise<TickersResponse> {
	const headers: Record<string, string> = { "Content-Type": "application/json" };
	if (API_KEY) {
		headers["X-API-KEY"] = API_KEY;
	}
	const response = await fetch(`${API_BASE}/market/tickers`, {
		method: "GET",
		headers
	});

	if (!response.ok) {
		throw new Error("Failed to fetch tickers");
	}

	return response.json();
}

export async function getIVSurface(ticker: string, surface: "mid" | "bid" | "ask"): Promise<IVSurfaceResponse> {
	const headers: Record<string, string> = { "Content-Type": "application/json" };
	if (API_KEY) {
		headers["X-API-KEY"] = API_KEY;
	}
	const response = await fetch(
		`${API_BASE}/market/iv/surface?ticker=${encodeURIComponent(ticker)}&surface=${surface}`,
		{
			method: "GET",
			headers
		}
	);

	if (!response.ok) {
		throw new Error("Failed to fetch IV surface");
	}

	return response.json();
}

export async function getRatesCurve(
	curve: "Treasury" | "SOFR" | "FedFunds",
	curveType: RatesCurveType = "zero",
	fixedPeriodYears = 0.5
): Promise<RatesCurveResponse> {
	const headers: Record<string, string> = { "Content-Type": "application/json" };
	if (API_KEY) {
		headers["X-API-KEY"] = API_KEY;
	}
	const response = await fetch(
		`${API_BASE}/market/rates/curve?curve=${encodeURIComponent(curve)}&curve_type=${encodeURIComponent(curveType)}&fixed_period_years=${fixedPeriodYears}`,
		{
			method: "GET",
			headers
		}
	);

	if (!response.ok) {
		throw new Error("Failed to fetch rates curve");
	}

	return response.json();
}

export async function getCleanedIVSurface(ticker: string): Promise<SurfaceGridResponse> {
	const headers: Record<string, string> = { "Content-Type": "application/json" };
	if (API_KEY) headers["X-API-KEY"] = API_KEY;
	const response = await fetch(
		`${API_BASE}/api/local-vol/iv-surface?ticker=${encodeURIComponent(ticker)}`,
		{ method: "GET", headers }
	);
	if (!response.ok) throw new Error("Failed to fetch cleaned IV surface");
	return response.json();
}

export async function getLocalVolSurface(ticker: string): Promise<SurfaceGridResponse> {
	const headers: Record<string, string> = { "Content-Type": "application/json" };
	if (API_KEY) headers["X-API-KEY"] = API_KEY;
	const response = await fetch(
		`${API_BASE}/api/local-vol/surface?ticker=${encodeURIComponent(ticker)}`,
		{ method: "GET", headers }
	);
	if (!response.ok) throw new Error("Failed to fetch local vol surface");
	return response.json();
}

// ── Portfolio types ──────────────────────────────────────────────────────────

export type PositionGreeks = {
	delta?: number | null;
	gamma?: number | null;
	vega?: number | null;
	theta?: number | null;
	rho?: number | null;
};

export type PositionResult = {
	npv: number;
	unit_price: number;
	greeks: PositionGreeks;
	priced_at?: string | null;
	engine: string;
	diagnostics: string;
	mc_std_error: number;
};

export type Position = {
	id: string;
	label: string;
	product_type: string;
	category: string;
	direction: "long" | "short";
	quantity: number;
	entry_price: number;
	parameters: Record<string, unknown>;
	result?: PositionResult | null;
};

export type Portfolio = {
	id: string;
	name: string;
	owner?: string;
	created_at: string;
	updated_at: string;
	positions: Position[];
};

export type PortfolioSummary = {
	id: string;
	name: string;
	created_at: string;
	updated_at: string;
	n_positions: number;
	total_value: number;
};

export type PortfolioRiskSummary = {
	total_npv: number;
	total_pnl: number;
	total_delta: number;
	total_gamma: number;
	total_vega: number;
	total_theta: number;
	total_rho: number;
	positions_priced: number;
	positions_total: number;
};

export type BatchPriceResponse = {
	portfolio: Portfolio;
	risk_summary: PortfolioRiskSummary;
};

export type StressBump = {
	name: string;
	spot_shift: number;
	vol_shift: number;
	rate_shift: number;
};

export type StressResult = {
	name: string;
	portfolio_pnl: number;
	position_pnls: Record<string, number>;
};

export type VaRResult = {
	confidence: number;
	horizon_days: number;
	var: number;
	expected_shortfall: number;
	method: string;
};

// ── Portfolio API ────────────────────────────────────────────────────────────

export function listPortfolios(): Promise<PortfolioSummary[]> {
	return apiFetch(`${API_BASE}/api/portfolios`, { headers: headers() });
}

export function createPortfolio(name: string): Promise<Portfolio> {
	return apiFetch(`${API_BASE}/api/portfolios?name=${encodeURIComponent(name)}`, {
		method: "POST",
		headers: headers(),
	});
}

export function getPortfolio(id: string): Promise<Portfolio> {
	return apiFetch(`${API_BASE}/api/portfolios/${id}`, { headers: headers() });
}

export function updatePortfolio(pf: Portfolio): Promise<Portfolio> {
	return apiFetch(`${API_BASE}/api/portfolios/${pf.id}`, {
		method: "PUT",
		headers: headers(),
		body: JSON.stringify(pf),
	});
}

export function deletePortfolio(id: string): Promise<void> {
	return apiFetch(`${API_BASE}/api/portfolios/${id}`, {
		method: "DELETE",
		headers: headers(),
	});
}

export function addPosition(portfolioId: string, pos: Position): Promise<Portfolio> {
	return apiFetch(`${API_BASE}/api/portfolios/${portfolioId}/positions`, {
		method: "POST",
		headers: headers(),
		body: JSON.stringify(pos),
	});
}

export function updatePosition(portfolioId: string, pos: Position): Promise<Portfolio> {
	return apiFetch(`${API_BASE}/api/portfolios/${portfolioId}/positions/${pos.id}`, {
		method: "PUT",
		headers: headers(),
		body: JSON.stringify(pos),
	});
}

export function removePosition(portfolioId: string, positionId: string): Promise<Portfolio> {
	return apiFetch(`${API_BASE}/api/portfolios/${portfolioId}/positions/${positionId}`, {
		method: "DELETE",
		headers: headers(),
	});
}

export function pricePortfolio(portfolioId: string): Promise<BatchPriceResponse> {
	return apiFetch(`${API_BASE}/api/portfolios/${portfolioId}/price`, {
		method: "POST",
		headers: headers(),
	});
}

export function stressTestPortfolio(portfolioId: string, bumps: StressBump[]): Promise<StressResult[]> {
	return apiFetch(`${API_BASE}/api/portfolios/${portfolioId}/stress`, {
		method: "POST",
		headers: headers(),
		body: JSON.stringify(bumps),
	});
}

export function computeVaR(
	portfolioId: string,
	confidence = 0.95,
	horizonDays = 1
): Promise<VaRResult> {
	return apiFetch(`${API_BASE}/api/portfolios/${portfolioId}/var`, {
		method: "POST",
		headers: headers(),
		body: JSON.stringify({
			portfolio_id: portfolioId,
			confidence,
			horizon_days: horizonDays,
		}),
	});
}

// ── Auth API ─────────────────────────────────────────────────────────────────

export type AuthResponse = {
	token: string;
	username: string;
};

export type UserInfo = {
	username: string;
};

export function loginUser(username: string, password: string): Promise<AuthResponse> {
	return apiFetch(`${API_BASE}/api/auth/login`, {
		method: "POST",
		headers: headers(),
		body: JSON.stringify({ username, password }),
	});
}

export function registerUser(username: string, password: string): Promise<AuthResponse> {
	return apiFetch(`${API_BASE}/api/auth/register`, {
		method: "POST",
		headers: headers(),
		body: JSON.stringify({ username, password }),
	});
}

export function getMe(token: string): Promise<UserInfo> {
	return apiFetch(`${API_BASE}/api/auth/me`, {
		headers: {
			...headers(),
			Authorization: `Bearer ${token}`,
		},
	});
}

// ── Backtest types ────────────────────────────────────────────────────────────

export type BacktestRequest = {
	tickers: string[];
	opt_start: string;   // ISO date "YYYY-MM-DD"
	opt_end: string;     // ISO date "YYYY-MM-DD"
	initial_capital: number;
	max_share: number;
	min_share: number;
	rebalance_freq: number;
};

export type AllocationRow = {
	ticker: string;
	weight: number;
	start_price: number;
	end_price: number;
	return_pct: number;
};

export type PortfolioPoint = {
	date: string;
	value: number;
};

export type BacktestMetrics = {
	ath: number;
	atl: number;
	total_return_pct: number;
	annualized_return_pct: number;
	max_drawdown: number;
	sharpe_ratio: number;
	optimal_sharpe: number;
	alpha: number | null;
	beta: number | null;
};

export type BacktestResponse = {
	opt_start: string;
	opt_end: string;
	allocation: AllocationRow[];
	portfolio_values: PortfolioPoint[];
	sp500_values: PortfolioPoint[] | null;
	metrics: BacktestMetrics;
	warnings: string[];
};

// ── Backtest API ──────────────────────────────────────────────────────────────

export function runBacktest(req: BacktestRequest): Promise<BacktestResponse> {
	return apiFetch(`${API_BASE}/api/backtest/run`, {
		method: "POST",
		headers: headers(),
		body: JSON.stringify(req),
	});
}
