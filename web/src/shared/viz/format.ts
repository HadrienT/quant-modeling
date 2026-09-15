/** Axis / value formatters per magnitude — blueprint WP 06 §3. */

export const fmt = {
	price: (v: number) => v.toFixed(2),
	pct: (v: number) => `${(v * 100).toFixed(1)}%`,
	volPts: (v: number) => `${(v * 100).toFixed(1)}`,
	signedPct: (v: number) =>
		`${v >= 0 ? "+" : "−"}${Math.abs(v * 100).toFixed(1)}%`,
	compact: (v: number) =>
		new Intl.NumberFormat("en-US", {
			notation: "compact",
			maximumFractionDigits: 1,
		}).format(v),
	years: (v: number) => (v < 1 ? `${Math.round(v * 12)}m` : `${v}y`),
	index100: (v: number) => v.toFixed(1),
} as const;

/** "Nice" domain with a little headroom, keeping zero when the sign matters.
 *
 * `clipOutliers` swaps the true min/max for the 5th/95th percentile before
 * padding -- for a handful of quantities (risk reversal / butterfly across
 * maturities) a single degenerate point (an illiquid near-zero-DTE slice
 * whose delta-bucket solve barely converges) can be an order of magnitude
 * outside the rest of the series, and a plain min/max domain then crushes
 * every legitimate value flat against the zero line, same failure mode
 * `robustZExtent` fixed for the 3D surface. Off by default: most callers
 * (a smile, a Greek profile) want the true extent, where "the line touches
 * the edge" is itself informative. Needs at least 5 points to be meaningful;
 * below that it falls back to the plain extent. */
export function niceDomain(
	values: number[],
	{
		includeZero = false,
		clipOutliers = false,
	}: { includeZero?: boolean; clipOutliers?: boolean } = {},
): [number, number] {
	let min: number;
	let max: number;
	if (clipOutliers && values.length >= 5) {
		const sorted = [...values].sort((a, b) => a - b);
		const at = (q: number) =>
			sorted[Math.min(sorted.length - 1, Math.max(0, Math.round(q * (sorted.length - 1))))]!;
		min = at(0.05);
		max = at(0.95);
		if (min === max) {
			min = Math.min(...values);
			max = Math.max(...values);
		}
	} else {
		min = Math.min(...values);
		max = Math.max(...values);
	}
	if (includeZero) {
		min = Math.min(min, 0);
		max = Math.max(max, 0);
	}
	if (min === max) {
		const pad = Math.abs(min) || 1;
		return [min - pad, max + pad];
	}
	const pad = (max - min) * 0.06;
	return [min - pad, max + pad];
}
