import { Cartesian } from "../Cartesian";
import { ChartFrame } from "../ChartFrame";
import { useChartTheme } from "../theme";
import { tickDate, yearFraction } from "../format";

const pct = (v: number) => `${Math.round(v * 100)}%`;

/**
 * What was held through time: the weights decided on each date, stacked, each
 * drawn until the next decision. Rectangles, not a smoothed area: a weight is
 * decided on a date and nothing is decided in between.
 */
export function WeightsChart({
	dates,
	end,
	labels,
	weights,
	title,
	subtitle,
	height = 240,
}: {
	/** Decision dates. */
	dates: string[];
	/** Last day of the run: where the last decision stops being held. */
	end: string;
	labels: string[];
	/** One row per date, one weight per label. */
	weights: number[][];
	title?: string;
	subtitle?: string;
	height?: number;
}) {
	const t = useChartTheme();
	const x = [...dates.map(yearFraction), yearFraction(end)];
	const color = (k: number) => t.series[k % t.series.length]!;

	return (
		<ChartFrame
			title={title}
			subtitle={subtitle}
			isEmpty={dates.length === 0}
			height={height}
			legend={labels.map((label, k) => ({ label, color: color(k) }))}
			table={{
				columns: ["Date", ...labels],
				rows: dates.map((d, i) => [
					d,
					...weights[i]!.map((w) => `${(w * 100).toFixed(1)}%`),
				]),
			}}
		>
			<Cartesian
				height={height}
				margin={{ top: 12, right: 16, bottom: 28, left: 56 }}
				xDomain={[x[0] ?? 0, x[x.length - 1] ?? 1]}
				yDomain={[0, 1]}
				yFormat={pct}
				xFormat={tickDate}
			>
				{({ xScale, yScale }) => (
					<>
						{weights.map((row, i) => {
							let below = 0;
							return row.map((w, k) => {
								const base = below;
								below += Math.max(0, w);
								if (w <= 0) return null;
								return (
									<rect
										key={`${i}-${k}`}
										x={xScale(x[i]!)}
										width={Math.max(0, xScale(x[i + 1]!) - xScale(x[i]!))}
										y={yScale(below)}
										height={Math.max(0, yScale(base) - yScale(below))}
										fill={color(k)}
										opacity={0.85}
									/>
								);
							});
						})}
					</>
				)}
			</Cartesian>
		</ChartFrame>
	);
}
