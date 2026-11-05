import { describe, expect, it } from "vitest";
import { type MeasuredPoint, fitSlope, slopeLine } from "./convergenceSlope";

const law = (power: number, ns = [1e3, 2e3, 4e3, 8e3]): MeasuredPoint[] =>
	ns.map((n) => ({ paths: n, npv: 0, std_error: 3 * n ** power }));

describe("fitSlope", () => {
	it("recovers an exact power law, with no error", () => {
		const fit = fitSlope(law(-0.8));
		expect(fit?.slope).toBeCloseTo(-0.8, 10);
		expect(fit?.stdError).toBeCloseTo(0, 10);
	});

	it("needs three points with a non-zero error", () => {
		expect(fitSlope(law(-0.5).slice(0, 2))).toBeNull();
		const flat = law(-0.5).map((p) => ({ ...p, std_error: 0 }));
		expect(fitSlope(flat)).toBeNull();
	});
});

describe("slopeLine", () => {
	const fit = (slope: number, stdError: number) => ({ slope, stdError });

	it("reads the slope against 1/√N at two standard errors of the fit", () => {
		expect(slopeLine(fit(-0.503, 0.005))).toContain("consistent with 1/√N");
		expect(slopeLine(fit(-0.865, 0.072))).toContain("faster than 1/√N");
		expect(slopeLine(fit(-0.3, 0.02))).toContain("slower than 1/√N");
	});

	it("prints a real minus sign and the regression error", () => {
		expect(slopeLine(fit(-0.503, 0.0048))).toContain("slope −0.503 ± 0.005");
	});

	it("claims no slope it could not fit", () => {
		expect(slopeLine(null)).toContain("too few points");
	});
});
