import { LinePath } from "@visx/shape";
import { curveMonotoneX } from "@visx/curve";
import { Cartesian } from "../Cartesian";
import { ChartFrame } from "../ChartFrame";
import { niceDomain } from "../format";
import { useChartTheme } from "../theme";

export type SkewPoint = {
	ttm: number;
	rr25: number | null;
	bf25: number | null;
	rr10: number | null;
	bf10: number | null;
};

const SERIES = [
	{ key: "rr25" as const, label: "RR25" },
	{ key: "bf25" as const, label: "BF25" },
	{ key: "rr10" as const, label: "RR10" },
	{ key: "bf10" as const, label: "BF10" },
];

/** Split into contiguous runs wherever a series' value is null -- a
 * 10-delta bucket that a given maturity's smile can't reach (see
 * vol_surface.solve_k_for_delta) is a genuine gap, not something to
 * interpolate across, same rationale as SmileChart's hole handling. */
function segments(
	points: SkewPoint[],
	key: (typeof SERIES)[number]["key"],
): [number, number][][] {
	const out: [number, number][][] = [];
	let cur: [number, number][] = [];
	for (const p of points) {
		const v = p[key];
		if (v == null) {
			if (cur.length) out.push(cur);
			cur = [];
		} else cur.push([p.ttm, v]);
	}
	if (cur.length) out.push(cur);
	return out;
}

/**
 * Skew and convexity, quoted the way a desk actually quotes them: risk
 * reversal (RR = vol(delta call) - vol(delta put), the slope of the smile)
 * and butterfly (BF = avg(call, put) - ATM, its curvature), at 25- and
 * 10-delta, against maturity -- not the smile's raw shape at one tenor.
 */
export function RiskReversalButterflyChart({
	points,
	title = "Risk reversal / butterfly",
	isLoading,
	error,
}: {
	points?: SkewPoint[];
	title?: string;
	isLoading?: boolean;
	error?: unknown;
}) {
	const t = useChartTheme();
	const isEmpty = !points || points.length === 0;

	const allY =
		points?.flatMap((p) => [p.rr25, p.bf25, p.rr10, p.bf10]).filter((v): v is number => v != null) ?? [];

	return (
		<ChartFrame
			title={title}
			subtitle="RR = vol(Δc) − vol(Δp) · BF = avg(Δc,Δp) − ATM"
			isLoading={isLoading}
			error={error}
			isEmpty={isEmpty}
			height={240}
			legend={SERIES.map((s, i) => ({
				label: s.label,
				color: t.series[i % t.series.length]!,
			}))}
			table={
				points
					? {
							columns: ["Maturity (y)", "RR25", "BF25", "RR10", "BF10"],
							rows: points.map((p) => [
								p.ttm.toFixed(3),
								p.rr25 != null ? `${(p.rr25 * 100).toFixed(2)}%` : "—",
								p.bf25 != null ? `${(p.bf25 * 100).toFixed(2)}%` : "—",
								p.rr10 != null ? `${(p.rr10 * 100).toFixed(2)}%` : "—",
								p.bf10 != null ? `${(p.bf10 * 100).toFixed(2)}%` : "—",
							]),
						}
					: undefined
			}
		>
			{points && (
				<Cartesian
					height={240}
					xDomain={niceDomain(points.map((p) => p.ttm), { includeZero: true })}
					yDomain={niceDomain(allY, { includeZero: true })}
					xLabel="Maturity (years)"
					yFormat={(v) => `${(v * 100).toFixed(1)}%`}
					xFormat={(v) => v.toFixed(1)}
					zeroLine
				>
					{({ xScale, yScale }) => (
						<>
							{SERIES.map((s, i) =>
								segments(points, s.key).map((seg, k) => (
									<LinePath
										key={`${s.key}-${k}`}
										data={seg}
										x={(d) => xScale(d[0]!)}
										y={(d) => yScale(d[1]!)}
										stroke={t.series[i % t.series.length]}
										strokeWidth={1.75}
										curve={curveMonotoneX}
									/>
								)),
							)}
						</>
					)}
				</Cartesian>
			)}
		</ChartFrame>
	);
}
