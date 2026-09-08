import { describe, expect, it } from "vitest";
import { niceDomain } from "./format";

describe("niceDomain", () => {
	it("adds headroom without touching zero by default", () => {
		const [min, max] = niceDomain([10, 20, 30]);
		expect(min).toBeLessThan(10);
		expect(max).toBeGreaterThan(30);
		expect(min).toBeGreaterThan(0);
	});

	it("includes zero when the sign matters (P&L, greeks)", () => {
		const [min, max] = niceDomain([5, 12, 20], { includeZero: true });
		expect(min).toBeLessThanOrEqual(0);
		expect(max).toBeGreaterThan(20);
	});

	it("handles a degenerate single-value range", () => {
		const [min, max] = niceDomain([7, 7]);
		expect(min).toBeLessThan(7);
		expect(max).toBeGreaterThan(7);
	});
});
