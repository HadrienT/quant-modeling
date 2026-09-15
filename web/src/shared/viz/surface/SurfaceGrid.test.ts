import { describe, expect, it } from "vitest";
import { makeGrid, robustZExtent, zExtent } from "./SurfaceGrid";

const AXES = { x: { label: "x" }, y: { label: "y" }, z: { label: "z" } };

describe("robustZExtent", () => {
	it("matches the true extent when the data has no outliers", () => {
		const x = [0, 1, 2, 3];
		const y = [0, 1, 2, 3];
		const z = y.map(() => x.map((xi) => 0.2 + xi * 0.01));
		const grid = makeGrid(x, y, z, AXES);
		expect(robustZExtent(grid)).toEqual(zExtent(grid));
	});

	it("is not dominated by a single extreme cell (the near-zero-DTE wing bug)", () => {
		// 99 cells tightly clustered around 24-26%, one cell at 290% (a
		// short-dated SVI wing artifact) -- exactly the AAPL case that
		// motivated this fix (docs/geometry.ts's doc comment).
		const x = Array.from({ length: 10 }, (_, i) => i);
		const y = Array.from({ length: 10 }, (_, i) => i);
		const z = y.map((_, yi) =>
			x.map((_, xi) => (xi === 0 && yi === 0 ? 2.9 : 0.24 + (xi + yi) * 0.001)),
		);
		const grid = makeGrid(x, y, z, AXES);

		const [trueMin, trueMax] = zExtent(grid);
		expect(trueMax).toBeCloseTo(2.9, 5);

		const [robustMin, robustMax] = robustZExtent(grid);
		expect(robustMax).toBeLessThan(0.3); // the outlier no longer sets the ceiling
		expect(robustMin).toBeGreaterThanOrEqual(trueMin);
		expect(robustMax).toBeLessThan(trueMax);
	});

	it("falls back to the true extent when clipping would collapse the range", () => {
		// A grid with almost every cell equal: p2 and p98 land on the same
		// value, which would otherwise divide-by-zero downstream.
		const x = [0, 1];
		const y = [0, 1];
		const grid = makeGrid(x, y, [[0.2, 0.2], [0.2, 0.2]], AXES);
		expect(robustZExtent(grid)).toEqual(zExtent(grid));
	});

	it("ignores holes", () => {
		const x = [0, 1, 2];
		const y = [0, 1, 2];
		const z = [
			[0.2, null, 0.24],
			[0.22, 0.23, null],
			[null, 0.21, 0.25],
		];
		const grid = makeGrid(x, y, z, AXES);
		const [lo, hi] = robustZExtent(grid);
		expect(lo).toBeGreaterThanOrEqual(0.2);
		expect(hi).toBeLessThanOrEqual(0.25);
	});
});
