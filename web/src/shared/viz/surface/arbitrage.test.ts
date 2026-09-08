import { describe, expect, it } from "vitest";
import { makeGrid } from "./SurfaceGrid";
import { checkArbitrage, coverage } from "./arbitrage";

const strikes = [80, 90, 100, 110, 120];
const mats = [0.25, 0.5, 1, 2];

describe("checkArbitrage (on total variance, not vol)", () => {
	it("passes a well-behaved flat surface", () => {
		const g = makeGrid(
			strikes,
			mats,
			mats.map(() => strikes.map(() => 0.2)),
			{ x: { label: "K" }, y: { label: "T" }, z: { label: "iv" } },
		);
		expect(checkArbitrage(g)).toHaveLength(0);
	});

	it("flags a calendar-arbitrage dip in total variance", () => {
		// σ drops enough between T=0.5 and T=1 that σ²T decreases
		const z = mats.map((t) => strikes.map(() => (t === 1 ? 0.1 : 0.2)));
		const g = makeGrid(strikes, mats, z, {
			x: { label: "K" },
			y: { label: "T" },
			z: { label: "iv" },
		});
		const v = checkArbitrage(g);
		expect(v.some((x) => x.kind === "calendar")).toBe(true);
	});

	it("flags a butterfly (concave-in-strike) violation", () => {
		const z = mats.map(() => [0.2, 0.2, 0.35, 0.2, 0.2]); // spike at ATM
		const g = makeGrid(strikes, mats, z, {
			x: { label: "K" },
			y: { label: "T" },
			z: { label: "iv" },
		});
		expect(checkArbitrage(g).some((x) => x.kind === "butterfly")).toBe(true);
	});
});

describe("coverage", () => {
	it("reports the fraction of nodes with a real quote", () => {
		const z = mats.map((_, i) =>
			strikes.map((_, j) => (i === 0 && j === 0 ? null : 0.2)),
		);
		const g = makeGrid(strikes, mats, z, {
			x: { label: "K" },
			y: { label: "T" },
			z: { label: "iv" },
		});
		expect(coverage(g)).toBeCloseTo(19 / 20);
	});
});
