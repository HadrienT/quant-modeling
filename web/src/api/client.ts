const API_BASE = import.meta.env.VITE_API_BASE ?? "http://localhost:8000";

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
	const response = await fetch(`${API_BASE}/price/vanilla`, {
		method: "POST",
		headers: { "Content-Type": "application/json" },
		body: JSON.stringify(payload)
	});

	if (!response.ok) {
		throw new Error("Failed to fetch vanilla price");
	}

	return response.json();
}
