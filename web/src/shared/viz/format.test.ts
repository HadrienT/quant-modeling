import { describe, expect, it } from "vitest";
import { logTicks, niceDomain, stepFormat } from "./format";

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

describe("stepFormat", () => {
	it("gives every tick the decimals of the step, so no two ticks share a label", () => {
		const f = stepFormat([0, 0.02], 4);
		const labels = [0, 0.005, 0.01, 0.015, 0.02].map(f);
		expect(labels).toEqual(["0.000", "0.005", "0.010", "0.015", "0.020"]);
	});

	it("drops decimals a coarse step does not need", () => {
		expect(stepFormat([0, 40], 4)(30)).toBe("30");
	});

	it("never writes zero in exponent notation or with a minus sign", () => {
		expect(stepFormat([-6, 0], 4)(-1e-17)).toBe("0");
	});
});

describe("logTicks", () => {
	it("puts 1, 2 and 5 times each power of ten inside a narrow domain", () => {
		expect(logTicks([7e-4, 1.1e-2])).toEqual([1e-3, 2e-3, 5e-3, 1e-2]);
	});

	it("thins to one tick per decade over many decades", () => {
		expect(logTicks([1e3, 1e7])).toEqual([1e3, 1e4, 1e5, 1e6, 1e7]);
	});

	it("labels the two ends of a domain narrower than one step", () => {
		expect(logTicks([3e-3, 4e-3])).toEqual([3e-3, 4e-3]);
	});
});
