import { LinePath } from "@visx/shape";
import { curveLinear } from "@visx/curve";
import { Cartesian } from "../Cartesian";
import { ChartFrame } from "../ChartFrame";
import { niceDomain } from "../format";
import { useChartTheme } from "../theme";

export type RateSeries = {
	label: string;
	/** x = tenor in years, y = rate as a decimal. */
	points: { x: number; y: number }[];
	/** Draw a dot at every point (quoted instruments). */
	markers?: boolean;
	/** false = dots only, no line (quotes are not joined by an interpolation). */
	line?: boolean;
};

const pct = (v: number) => `${(v * 100).toFixed(3)}%`;
const tenor = (v: number) =>
	v < 1 ? `${Math.round(v * 12)}m` : `${Math.round(v * 100) / 100}y`;

/**
 * Rate curves on a tenor axis (WP 06 §2). Segments are straight: a derived
 * curve arrives densely sampled with its own interpolation (rates.py), and a
 * plotting spline would draw values no curve contains. The table lists every
 * tenor of every series, values matched by tenor — not by position.
 */
export function RatesCurveChart({
	series,
	title = "Rates curve",
	isLoading,
	error,
	height = 280,
}: {
	series?: RateSeries[];
	title?: string;
	isLoading?: boolean;
	error?: unknown;
	height?: number;
}) {
	const t = useChartTheme();
	const all = series?.flatMap((s) => s.points) ?? [];
	const isEmpty = all.length === 0;
	const xs = [...new Set(all.map((p) => Math.round(p.x * 1e6) / 1e6))].sort(
		(a, b) => a - b,
	);
	const at = (s: RateSeries, x: number) =>
		s.points.find((p) => Math.abs(p.x - x) < 1e-6)?.y;

	return (
		<ChartFrame
			title={title}
			isLoading={isLoading}
			error={error}
			isEmpty={isEmpty}
			height={height}
			legend={series?.map((s, i) => ({
				label: s.label,
				color: t.series[i % t.series.length]!,
			}))}
			table={
				series?.length
					? {
							columns: ["Tenor", ...series.map((s) => s.label)],
							rows: xs.map((x) => [
								tenor(x),
								...series.map((s) => {
									const y = at(s, x);
									return y === undefined ? "—" : pct(y);
								}),
							]),
						}
					: undefined
			}
		>
			{series && (
				<Cartesian
					height={height}
					xDomain={niceDomain(all.map((p) => p.x))}
					yDomain={niceDomain(all.map((p) => p.y))}
					xLabel="Tenor (years)"
					yFormat={(v) => `${(v * 100).toFixed(2)}%`}
					xFormat={tenor}
				>
					{({ xScale, yScale }) =>
						series.map((s, i) => {
							const color = t.series[i % t.series.length];
							return (
								<g key={s.label}>
									{s.line !== false && (
										<LinePath
											data={s.points}
											x={(d) => xScale(d.x)}
											y={(d) => yScale(d.y)}
											stroke={color}
											strokeWidth={1.75}
											curve={curveLinear}
										/>
									)}
									{(s.markers || s.line === false) &&
										s.points.map((p) => (
											<circle
												key={p.x}
												cx={xScale(p.x)}
												cy={yScale(p.y)}
												r={3}
												fill={color}
											>
												<title>{`${s.label} ${tenor(p.x)}: ${pct(p.y)}`}</title>
											</circle>
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
