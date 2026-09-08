import { LinePath } from "@visx/shape";
import { curveMonotoneX } from "@visx/curve";
import { Cartesian } from "../Cartesian";
import { ChartFrame } from "../ChartFrame";
import { niceDomain } from "../format";
import { useChartTheme } from "../theme";

export type SmileSlice = {
	label: string;
	x: number[]; // strike or moneyness
	iv: (number | null)[];
};

/**
 * A slice through the vol surface (WP 06 §2). MUST share scales & colours with
 * the 3D renderer (WP 05) — the caller passes an explicit yDomain when syncing.
 * Holes (null) break the line, never interpolated.
 */
export function SmileChart({
	slices,
	xLabel = "Strike",
	yDomain,
	title = "Volatility smile",
	isLoading,
	error,
}: {
	slices?: SmileSlice[];
	xLabel?: string;
	yDomain?: [number, number];
	title?: string;
	isLoading?: boolean;
	error?: unknown;
}) {
	const t = useChartTheme();
	const isEmpty = !slices || slices.length === 0;
	const allX = slices?.flatMap((s) => s.x) ?? [];
	const allY =
		slices?.flatMap((s) => s.iv.filter((v): v is number => v != null)) ?? [];

	return (
		<ChartFrame
			title={title}
			isLoading={isLoading}
			error={error}
			isEmpty={isEmpty}
			height={260}
			legend={slices?.map((s, i) => ({
				label: s.label,
				color: t.series[i % t.series.length]!,
			}))}
		>
			{slices && (
				<Cartesian
					height={260}
					xDomain={niceDomain(allX)}
					yDomain={yDomain ?? niceDomain(allY)}
					xLabel={xLabel}
					yFormat={(v) => `${(v * 100).toFixed(0)}%`}
					xFormat={(v) => v.toFixed(0)}
				>
					{({ xScale, yScale }) =>
						slices.map((s, i) => {
							// split at nulls
							const segments: [number, number][][] = [];
							let cur: [number, number][] = [];
							s.x.forEach((x, j) => {
								const v = s.iv[j];
								if (v == null) {
									if (cur.length) segments.push(cur);
									cur = [];
								} else cur.push([x, v]);
							});
							if (cur.length) segments.push(cur);
							return (
								<g key={s.label}>
									{segments.map((seg, k) => (
										<LinePath
											key={k}
											data={seg}
											x={(d) => xScale(d[0]!)}
											y={(d) => yScale(d[1]!)}
											stroke={t.series[i % t.series.length]}
											strokeWidth={1.75}
											curve={curveMonotoneX}
										/>
									))}
								</g>
							);
						})
					}
				</Cartesian>
			)}
		</ChartFrame>
	);
}
