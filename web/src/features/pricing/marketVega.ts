import { useMutation } from "@tanstack/react-query";
import { api, ApiError } from "@/shared/api";
import type { Schemas } from "@/shared/api/types";
import { makeGrid, type SurfaceGrid } from "@/shared/viz";

export type MarketVega = Schemas["MarketVegaResponse"];

/** POST /price/market-vega: vega by quoted option (the Dupire superbucket). */
export function useMarketVega() {
	return useMutation<MarketVega, ApiError, unknown>({
		mutationFn: async (body) => {
			const { data, error } = await api.POST("/price/market-vega", {
				body: body as never,
			});
			if (error !== undefined) throw ApiError.from(error);
			return data;
		},
	});
}

const BIN = 0.05;

/** The quote vegas as a surface: maturity x log-moneyness bins of width
 * BIN, each cell the sum of its quotes' vegas; a cell without a quote is a
 * hole, never interpolated. */
export function vegaSurface(v: MarketVega): SurfaceGrid {
	const bin = (k: number) => Math.round(k / BIN);
	const bins = [...new Set(v.quotes.map((q) => bin(q.log_moneyness)))].sort(
		(a, b) => a - b,
	);
	const ttms = v.maturities.map((m) => m.ttm);
	const z = ttms.map(() => bins.map(() => null as number | null));
	for (const q of v.quotes) {
		const yi = ttms.indexOf(q.ttm);
		const xi = bins.indexOf(bin(q.log_moneyness));
		if (yi >= 0 && xi >= 0) z[yi]![xi] = (z[yi]![xi] ?? 0) + q.vega;
	}
	return makeGrid(
		bins.map((b) => b * BIN),
		ttms,
		z,
		{
			x: { label: "Log-moneyness ln(K/F)", format: (x) => x.toFixed(2) },
			y: { label: "Maturity", unit: "y", format: (y) => y.toFixed(2) },
			z: { label: "Vega per vol point" },
		},
	);
}
