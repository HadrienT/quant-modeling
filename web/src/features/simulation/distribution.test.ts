import { describe, expect, it } from "vitest";
import type { DistributionResponse } from "@/shared/api/types";
import {
	binCentres,
	lognormalPdf,
	martingaleCheck,
	nearestIndex,
	quantileBands,
} from "./distribution";

describe("simulation diagnostics helpers", () => {
	it("lognormal pdf integrates to one and has the forward as its mean", () => {
		const F = 105,
			sigma = 0.3,
			t = 2;
		let mass = 0,
			mean = 0;
		const dx = 0.05;
		for (let x = dx / 2; x < 1000; x += dx) {
			const p = lognormalPdf(x, F, sigma, t);
			mass += p * dx;
			mean += x * p * dx;
		}
		expect(mass).toBeCloseTo(1, 4);
		expect(mean).toBeCloseTo(F, 2);
		expect(lognormalPdf(-1, F, sigma, t)).toBe(0);
	});

	it("pairs symmetric quantiles outermost first, with the median", () => {
		const levels = [0.05, 0.25, 0.5, 0.75, 0.95];
		const q = levels.map((l) => [l]);
		const { bands, median } = quantileBands(levels, q);
		expect(bands.map((b) => b.label)).toEqual(["5–95%", "25–75%"]);
		expect(bands[0]!.lo).toEqual([0.05]);
		expect(bands[0]!.hi).toEqual([0.95]);
		expect(median).toEqual([0.5]);
	});

	it("finds the worst martingale deviation in standard errors", () => {
		const d = {
			martingale: {
				mean: [100, 99.9, 101.0],
				stderr: [0, 0.1, 0.5],
				exact: [100, 100, 100],
			},
		} as unknown as DistributionResponse;
		expect(martingaleCheck(d)).toEqual({ z: 2, index: 2 });
	});

	it("bins and grid lookups", () => {
		expect(binCentres(0, 10, 5)).toEqual([1, 3, 5, 7, 9]);
		expect(nearestIndex([0, 0.25, 0.5, 0.75, 1], 0.6)).toBe(2);
	});
});
