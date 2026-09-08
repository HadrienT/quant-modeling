import { describe, expect, it } from "vitest";
import { makeGrid, zExtent } from "./SurfaceGrid";
import { buildSurfaceGeometry, nearestNode } from "./geometry";

function gaussian(nx: number, ny: number, holeMask?: (xi: number, yi: number) => boolean) {
	const x = Array.from({ length: nx }, (_, i) => i);
	const y = Array.from({ length: ny }, (_, i) => i);
	const z = y.map((_, yi) =>
		x.map((_, xi) => {
			if (holeMask?.(xi, yi)) return null;
			const dx = (xi - nx / 2) / nx;
			const dy = (yi - ny / 2) / ny;
			return Math.exp(-(dx * dx + dy * dy) * 8);
		}),
	);
	return makeGrid(x, y, z, {
		x: { label: "x" },
		y: { label: "y" },
		z: { label: "z" },
	});
}

describe("buildSurfaceGeometry", () => {
	it("emits one vertex per grid node and 6 indices per interior quad", () => {
		const g = buildSurfaceGeometry(gaussian(5, 4));
		expect(g.positions.length).toBe(5 * 4 * 3);
		expect(g.indices.length).toBe((5 - 1) * (4 - 1) * 6);
	});

	it("does not index any quad touching a hole (holes are absence, not a bump)", () => {
		const withHole = gaussian(6, 6, (xi, yi) => xi === 3 && yi === 3);
		const g = buildSurfaceGeometry(withHole);
		// the hole vertex participates in 4 quads → 4 quads dropped
		const full = buildSurfaceGeometry(gaussian(6, 6));
		expect(g.indices.length).toBe(full.indices.length - 4 * 6);
	});

	it("holes carry the -1 hole flag in the value channel, real nodes carry 0..1", () => {
		const g = buildSurfaceGeometry(gaussian(5, 5, (xi, yi) => xi === 0 && yi === 0));
		expect(g.uv[1]).toBe(-1); // node (0,0) value slot
		expect(Math.max(...g.uv.filter((_, i) => i % 2 === 1 && _ >= 0))).toBeLessThanOrEqual(1);
	});

	it("normals are unit length on real nodes", () => {
		const g = buildSurfaceGeometry(gaussian(8, 8));
		for (let i = 0; i < g.values.length; i++) {
			if (Number.isNaN(g.values[i]!)) continue;
			const len = Math.hypot(
				g.normals[i * 3]!,
				g.normals[i * 3 + 1]!,
				g.normals[i * 3 + 2]!,
			);
			expect(len).toBeCloseTo(1, 4);
		}
	});
});

describe("nearestNode", () => {
	it("returns the exact grid value at a node (hover reads non-interpolated data)", () => {
		const g = gaussian(9, 9);
		// centre of parametric space → centre node
		const node = nearestNode(g, 0, 0);
		expect(node.xi).toBe(4);
		expect(node.yi).toBe(4);
		const [min, max] = zExtent(g);
		expect(node.z).toBeGreaterThanOrEqual(min);
		expect(node.z).toBeLessThanOrEqual(max);
	});
});
