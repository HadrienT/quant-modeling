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

/** "Nice" domain with a little headroom, keeping zero when the sign matters. */
export function niceDomain(
	values: number[],
	{ includeZero = false }: { includeZero?: boolean } = {},
): [number, number] {
	let min = Math.min(...values);
	let max = Math.max(...values);
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
