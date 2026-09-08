import { type SurfaceGrid, zAt } from "./SurfaceGrid";

/**
 * Static no-arbitrage checks on an IV surface — blueprint WP 08 §2.
 * MUST be done on TOTAL VARIANCE w = σ²·T, not on the vol itself (WP 08 pitfalls
 * — checking σ directly produces false alarms).
 *
 * - calendar: total variance non-decreasing in maturity at each strike
 * - butterfly: total variance convex in strike at each maturity (a proxy for
 *   the density staying non-negative)
 */

export type ArbViolation = {
	kind: "calendar" | "butterfly";
	xi: number;
	yi: number;
	detail: string;
};

export function checkArbitrage(
	grid: SurfaceGrid,
	{ tol = 1e-6 }: { tol?: number } = {},
): ArbViolation[] {
	const nx = grid.x.length;
	const ny = grid.y.length;
	const w = (xi: number, yi: number) => {
		const sigma = zAt(grid, xi, yi);
		return Number.isNaN(sigma) ? NaN : sigma * sigma * grid.y[yi]!;
	};
	const out: ArbViolation[] = [];

	// calendar
	for (let xi = 0; xi < nx; xi++) {
		for (let yi = 1; yi < ny; yi++) {
			const a = w(xi, yi - 1);
			const b = w(xi, yi);
			if (Number.isNaN(a) || Number.isNaN(b)) continue;
			if (b < a - tol) {
				out.push({
					kind: "calendar",
					xi,
					yi,
					detail: `total variance drops ${(a - b).toFixed(4)} from ${grid.y[
						yi - 1
					]!.toFixed(2)}y to ${grid.y[yi]!.toFixed(2)}y`,
				});
			}
		}
	}

	// butterfly: discrete second derivative in strike ≥ 0
	for (let yi = 0; yi < ny; yi++) {
		for (let xi = 1; xi < nx - 1; xi++) {
			const l = w(xi - 1, yi);
			const m = w(xi, yi);
			const r = w(xi + 1, yi);
			if (Number.isNaN(l) || Number.isNaN(m) || Number.isNaN(r)) continue;
			const dl = grid.x[xi]! - grid.x[xi - 1]!;
			const dr = grid.x[xi + 1]! - grid.x[xi]!;
			const second = (r - m) / dr - (m - l) / dl;
			if (second < -tol * 10) {
				out.push({
					kind: "butterfly",
					xi,
					yi,
					detail: `total variance concave in strike at K=${grid.x[xi]!.toFixed(0)}, T=${grid.y[
						yi
					]!.toFixed(2)}y`,
				});
			}
		}
	}

	return out;
}

/** Fraction of grid nodes that carry a real quote. */
export function coverage(grid: SurfaceGrid): number {
	let real = 0;
	for (let i = 0; i < grid.z.length; i++) if (!Number.isNaN(grid.z[i]!)) real++;
	return grid.z.length ? real / grid.z.length : 0;
}
