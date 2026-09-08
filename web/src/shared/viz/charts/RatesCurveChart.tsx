import { LinePath } from "@visx/shape";
import { curveMonotoneX } from "@visx/curve";
import { Cartesian } from "../Cartesian";
import { ChartFrame } from "../ChartFrame";
import { niceDomain } from "../format";
import { useChartTheme } from "../theme";

export type RateSeries = {
	label: string;
	points: { x: number; y: number }[];
	markers?: boolean;
};

/** Zero and forward curves superimposed, instrument points marked (WP 06 §2). */
export function RatesCurveChart({
	series,
	title = "Rates curve",
	isLoading,
	error,
}: {
	series?: RateSeries[];
	title?: string;
	isLoading?: boolean;
	error?: unknown;
}) {
	const t = useChartTheme();
	const all = series?.flatMap((s) => s.points) ?? [];
	const isEmpty = all.length === 0;

	return (
		<ChartFrame
			title={title}
			isLoading={isLoading}
			error={error}
			isEmpty={isEmpty}
			height={280}
			legend={series?.map((s, i) => ({
				label: s.label,
				color: t.series[i % t.series.length]!,
			}))}
			table={
				series?.[0]
					? {
							columns: ["Tenor (y)", ...series.map((s) => s.label)],
							rows: series[0].points.map((p, i) => [
								p.x,
								...series.map(
									(s) => ((s.points[i]?.y ?? 0) * 100).toFixed(2) + "%",
								),
							]),
						}
					: undefined
			}
		>
			{series && (
				<Cartesian
					height={280}
					xDomain={niceDomain(all.map((p) => p.x))}
					yDomain={niceDomain(all.map((p) => p.y))}
					xLabel="Tenor (years)"
					yFormat={(v) => `${(v * 100).toFixed(1)}%`}
					xFormat={(v) => (v < 1 ? `${Math.round(v * 12)}m` : `${v}y`)}
				>
					{({ xScale, yScale }) =>
						series.map((s, i) => (
							<g key={s.label}>
								<LinePath
									data={s.points}
									x={(d) => xScale(d.x)}
									y={(d) => yScale(d.y)}
									stroke={t.series[i % t.series.length]}
									strokeWidth={1.75}
									curve={curveMonotoneX}
								/>
								{s.markers &&
									s.points.map((p) => (
										<circle
											key={p.x}
											cx={xScale(p.x)}
											cy={yScale(p.y)}
											r={2.5}
											fill={t.series[i % t.series.length]}
										/>
									))}
							</g>
						))
					}
				</Cartesian>
			)}
		</ChartFrame>
	);
}
