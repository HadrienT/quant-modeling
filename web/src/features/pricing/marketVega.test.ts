import { describe, expect, it } from "vitest";
import { type MarketVega, vegaSurface } from "./marketVega";

const q = (ttm: number, k: number, vega: number) => ({
	ttm,
	strike: 100 * Math.exp(k),
	log_moneyness: k,
	implied_vol: 0.2,
	vega,
	std_error: 0,
});

describe("vegaSurface", () => {
	it("sums quotes per maturity and moneyness bin, leaves empty cells as holes", () => {
		const v = {
			maturities: [
				{ ttm: 0.5, vega: 0, std_error: 0, n_quotes: 2 },
				{ ttm: 1, vega: 0, std_error: 0, n_quotes: 1 },
			],
			quotes: [q(0.5, 0.0, 1), q(0.5, 0.01, 2), q(1, 0.1, -3)],
		} as unknown as MarketVega;
		const g = vegaSurface(v);
		expect(Array.from(g.x)).toEqual([0, 0.1]);
		expect(Array.from(g.y)).toEqual([0.5, 1]);
		// row-major z[yi * nx + xi]
		expect(g.z[0]).toBe(3); // 0.5y, k ~ 0: 1 + 2
		expect(Number.isNaN(g.z[1]!)).toBe(true); // 0.5y, k 0.1: no quote
		expect(g.z[3]).toBe(-3);
	});
});
