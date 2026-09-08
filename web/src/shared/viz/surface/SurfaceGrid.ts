/**
 * Normalised surface grid — blueprint WP 05 §1, dependencies §3.5.
 * The renderer knows nothing about finance: implied vol, local vol, a P&L nappe
 * and (later) an exposure profile are the same mathematical object.
 *
 * NaN in `z` is a HOLE (a strike without a quote). Holes are rendered as absence
 * of material, never interpolated — a smooth surface where there is no data is a
 * lie about market quality.
 */

export type AxisSpec = {
	label: string;
	unit?: string;
	format?: (v: number) => string;
};

export type SurfaceGrid = {
	/** x breakpoints — e.g. strikes. Not necessarily regularly spaced. */
	x: Float64Array;
	/** y breakpoints — e.g. maturities. Not necessarily regular. */
	y: Float64Array;
	/** row-major, length x.length * y.length. NaN = hole. z[yi * x.length + xi] */
	z: Float64Array;
	axes: { x: AxisSpec; y: AxisSpec; z: AxisSpec };
};

export function makeGrid(
	x: number[],
	y: number[],
	z: (number | null | undefined)[][],
	axes: SurfaceGrid["axes"],
): SurfaceGrid {
	const nx = x.length;
	const flat = new Float64Array(nx * y.length);
	for (let yi = 0; yi < y.length; yi++) {
		for (let xi = 0; xi < nx; xi++) {
			const v = z[yi]?.[xi];
			flat[yi * nx + xi] = v == null ? NaN : v;
		}
	}
	return { x: Float64Array.from(x), y: Float64Array.from(y), z: flat, axes };
}

export function zAt(grid: SurfaceGrid, xi: number, yi: number): number {
	return grid.z[yi * grid.x.length + xi] ?? NaN;
}

/** Min / max over the non-hole values. */
export function zExtent(grid: SurfaceGrid): [number, number] {
	let min = Infinity;
	let max = -Infinity;
	for (let i = 0; i < grid.z.length; i++) {
		const v = grid.z[i]!;
		if (Number.isNaN(v)) continue;
		if (v < min) min = v;
		if (v > max) max = v;
	}
	return [min, max];
}

export function holeFraction(grid: SurfaceGrid): number {
	let holes = 0;
	for (let i = 0; i < grid.z.length; i++) if (Number.isNaN(grid.z[i]!)) holes++;
	return grid.z.length ? holes / grid.z.length : 0;
}
