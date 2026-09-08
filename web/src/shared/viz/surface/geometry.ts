import { type SurfaceGrid, zAt, zExtent } from "./SurfaceGrid";

/**
 * Build BufferGeometry attributes from a grid — blueprint WP 05 §2.
 * Pure, testable without WebGL. Geometry lives in a normalised parametric space
 * [-1,1]²; the AXIS TICKS carry the real irregular values, so the smile wings
 * are not distorted.
 *
 * - one Float32Array allocation per attribute
 * - normals computed ANALYTICALLY from neighbours (not computeVertexNormals) so
 *   holes do not pollute their neighbours' lighting
 * - quads with any NaN corner are simply not indexed (zero render cost)
 */

export type SurfaceGeometry = {
	positions: Float32Array; // 3 per vertex, x,z ∈ [-1,1], y = normalised height
	normals: Float32Array;
	uv: Float32Array; // 2 per vertex, for the colormap LUT lookup (v = value)
	indices: Uint32Array;
	/** grid value per vertex (NaN for holes) — for hover / isoline uniforms */
	values: Float32Array;
	zMin: number;
	zMax: number;
	heightScale: number;
};

export function buildSurfaceGeometry(
	grid: SurfaceGrid,
	{ heightScale = 0.5 }: { heightScale?: number } = {},
): SurfaceGeometry {
	const nx = grid.x.length;
	const ny = grid.y.length;
	const [zMin, zMax] = zExtent(grid);
	const span = zMax - zMin || 1;

	const positions = new Float32Array(nx * ny * 3);
	const uv = new Float32Array(nx * ny * 2);
	const values = new Float32Array(nx * ny);

	const px = (xi: number) => (nx === 1 ? 0 : (xi / (nx - 1)) * 2 - 1);
	const pz = (yi: number) => (ny === 1 ? 0 : (yi / (ny - 1)) * 2 - 1);

	for (let yi = 0; yi < ny; yi++) {
		for (let xi = 0; xi < nx; xi++) {
			const i = yi * nx + xi;
			const v = zAt(grid, xi, yi);
			const norm = Number.isNaN(v) ? 0 : (v - zMin) / span;
			positions[i * 3] = px(xi);
			positions[i * 3 + 1] = Number.isNaN(v) ? 0 : norm * heightScale;
			positions[i * 3 + 2] = pz(yi);
			uv[i * 2] = nx === 1 ? 0 : xi / (nx - 1);
			uv[i * 2 + 1] = Number.isNaN(v) ? -1 : norm; // -1 flags a hole in the shader
			values[i] = v;
		}
	}

	// Analytic normals via central differences on the parametric grid.
	const normals = new Float32Array(nx * ny * 3);
	const dxWorld = nx > 1 ? 2 / (nx - 1) : 1;
	const dzWorld = ny > 1 ? 2 / (ny - 1) : 1;
	for (let yi = 0; yi < ny; yi++) {
		for (let xi = 0; xi < nx; xi++) {
			const i = yi * nx + xi;
			if (Number.isNaN(values[i]!)) {
				normals[i * 3 + 1] = 1;
				continue;
			}
			const h = (xx: number, yy: number) => {
				const vv = zAt(grid, xx, yy);
				return Number.isNaN(vv) ? NaN : ((vv - zMin) / span) * heightScale;
			};
			const hL = h(Math.max(0, xi - 1), yi);
			const hR = h(Math.min(nx - 1, xi + 1), yi);
			const hD = h(xi, Math.max(0, yi - 1));
			const hU = h(xi, Math.min(ny - 1, yi + 1));
			const hi = h(xi, yi);
			const dhdx =
				(Number.isNaN(hR) ? hi : Number.isNaN(hL) ? hi : (hR - hL) / 2) /
				dxWorld;
			const dhdz =
				(Number.isNaN(hU) ? hi : Number.isNaN(hD) ? hi : (hU - hD) / 2) /
				dzWorld;
			// normal of surface y = h(x,z): (-dh/dx, 1, -dh/dz), normalised
			const len = Math.hypot(dhdx, 1, dhdz) || 1;
			normals[i * 3] = -dhdx / len;
			normals[i * 3 + 1] = 1 / len;
			normals[i * 3 + 2] = -dhdz / len;
		}
	}

	// Index only quads whose four corners are all real.
	const idx: number[] = [];
	for (let yi = 0; yi < ny - 1; yi++) {
		for (let xi = 0; xi < nx - 1; xi++) {
			const a = yi * nx + xi;
			const b = a + 1;
			const c = a + nx;
			const d = c + 1;
			if (
				Number.isNaN(values[a]!) ||
				Number.isNaN(values[b]!) ||
				Number.isNaN(values[c]!) ||
				Number.isNaN(values[d]!)
			)
				continue;
			idx.push(a, c, b, b, c, d);
		}
	}

	return {
		positions,
		normals,
		uv,
		indices: Uint32Array.from(idx),
		values,
		zMin,
		zMax,
		heightScale,
	};
}

/** Nearest grid node to a normalised (x,z) ∈ [-1,1]² — used by hover raycast. */
export function nearestNode(
	grid: SurfaceGrid,
	px: number,
	pz: number,
): { xi: number; yi: number; x: number; y: number; z: number } {
	const nx = grid.x.length;
	const ny = grid.y.length;
	const xi = Math.round(((px + 1) / 2) * (nx - 1));
	const yi = Math.round(((pz + 1) / 2) * (ny - 1));
	const cxi = Math.max(0, Math.min(nx - 1, xi));
	const cyi = Math.max(0, Math.min(ny - 1, yi));
	return {
		xi: cxi,
		yi: cyi,
		x: grid.x[cxi]!,
		y: grid.y[cyi]!,
		z: zAt(grid, cxi, cyi),
	};
}
