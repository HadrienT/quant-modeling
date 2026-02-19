const API_BASE = import.meta.env.VITE_API_BASE ?? "http://localhost:8000";
const API_KEY = import.meta.env.VITE_API_KEY;

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
	curve: "SOFR" | "OIS";
	zero: Array<{ x: number; y: number }>;
};

export type RatesCurveType = "zero" | "forward";

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
	curve: "SOFR" | "OIS",
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
