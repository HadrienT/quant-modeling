import type {
	CleanedIVSurfaceResponse,
	IVSurfaceResponse,
	LocalVolSurfaceResponse,
} from "@/shared/api/types";
import { type SurfaceGrid, makeGrid } from "./SurfaceGrid";

/**
 * API responses → SurfaceGrid (blueprint WP 05 §1). The renderer never sees an
 * API type. `null` in the API's Array<Array<number|null>> becomes a hole.
 */

const VOL_AXES = {
	x: { label: "Strike", format: (v: number) => v.toFixed(0) },
	y: { label: "Maturity", unit: "y", format: (v: number) => (v < 1 ? `${Math.round(v * 12)}m` : `${v}y`) },
	z: { label: "Implied vol", unit: "%", format: (v: number) => `${(v * 100).toFixed(1)}%` },
} as const;

export function ivSurfaceToGrid(r: IVSurfaceResponse): SurfaceGrid {
	return makeGrid(r.strikes, r.maturities, r.values, {
		x: { ...VOL_AXES.x },
		y: { ...VOL_AXES.y },
		z: { ...VOL_AXES.z, label: `Implied vol (${r.surface})` },
	});
}

export function cleanedIvSurfaceToGrid(r: CleanedIVSurfaceResponse): SurfaceGrid {
	return makeGrid(r.strikes, r.maturities, r.values, {
		x: { ...VOL_AXES.x },
		y: { ...VOL_AXES.y },
		z: { ...VOL_AXES.z, label: "Implied vol (cleaned)" },
	});
}

export function localVolSurfaceToGrid(r: LocalVolSurfaceResponse): SurfaceGrid {
	return makeGrid(r.strikes, r.maturities, r.values, {
		x: { ...VOL_AXES.x },
		y: { ...VOL_AXES.y },
		z: { ...VOL_AXES.z, label: "Local vol (Dupire)" },
	});
}

/** Difference of two grids on a shared mesh — vol locale − vol implicite (WP 05 §3). */
export function differenceGrid(a: SurfaceGrid, b: SurfaceGrid): SurfaceGrid {
	const z = new Float64Array(a.z.length);
	for (let i = 0; i < a.z.length; i++) {
		const va = a.z[i]!;
		const vb = b.z[i]!;
		z[i] = Number.isNaN(va) || Number.isNaN(vb) ? NaN : va - vb;
	}
	return {
		x: a.x,
		y: a.y,
		z,
		axes: { ...a.axes, z: { label: "Difference", unit: "%", format: (v) => `${(v * 100).toFixed(2)}%` } },
	};
}
