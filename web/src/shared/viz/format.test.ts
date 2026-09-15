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

	// A risk-reversal/butterfly term structure: ~20 maturities clustered
	// within a few percent, plus one degenerate near-zero-DTE slice whose
	// delta solve barely converged to a much larger value (the AAPL bug
	// that motivated clipOutliers).
	const cluster = Array.from({ length: 19 }, (_, i) => -0.05 + i * 0.005);
	const withOutlier = [...cluster, 4.9];

	it("without clipOutliers, one huge value stretches the whole domain", () => {
		const [, max] = niceDomain(withOutlier);
		expect(max).toBeGreaterThan(4);
	});

	it("clipOutliers keeps the domain tight around the bulk of the series", () => {
		const [min, max] = niceDomain(withOutlier, { clipOutliers: true });
		expect(max).toBeLessThan(1);
		expect(min).toBeGreaterThan(-1);
	});

	it("clipOutliers falls back to the plain extent below 5 points", () => {
		const values = [-0.01, 0.0, 4.9];
		const [, max] = niceDomain(values, { clipOutliers: true });
		expect(max).toBeGreaterThan(4);
	});
});
