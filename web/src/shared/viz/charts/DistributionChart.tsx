import { Bar } from "@visx/shape";
import { Cartesian } from "../Cartesian";
import { ChartFrame } from "../ChartFrame";
import { niceDomain } from "../format";
import { useChartTheme } from "../theme";

/**
 * P&L histogram with VaR and ES drawn as vertical lines (blueprint WP 06 §2).
 * `samples` are raw P&L values; binning happens here.
 */
export function DistributionChart({
	samples,
	varLevel,
	expectedShortfall,
	bins = 40,
	title = "P&L distribution",
	isLoading,
	error,
}: {
	samples?: number[];
	varLevel?: number;
	expectedShortfall?: number;
	bins?: number;
	title?: string;
	isLoading?: boolean;
	error?: unknown;
}) {
	const t = useChartTheme();
	const isEmpty = !samples || samples.length === 0;

	let hist: { x0: number; x1: number; n: number }[] = [];
	if (samples && samples.length) {
		const min = Math.min(...samples);
		const max = Math.max(...samples);
		const w = (max - min) / bins || 1;
		hist = Array.from({ length: bins }, (_, i) => ({
			x0: min + i * w,
			x1: min + (i + 1) * w,
			n: 0,
		}));
		for (const s of samples) {
			const idx = Math.min(bins - 1, Math.floor((s - min) / w));
			hist[idx]!.n++;
		}
	}

	return (
		<ChartFrame
			title={title}
			isLoading={isLoading}
			error={error}
			isEmpty={isEmpty}
			height={260}
			legend={[
				{ label: "frequency", color: t.series[0]! },
				...(varLevel != null
					? [{ label: "VaR", color: t.status.critical }]
					: []),
				...(expectedShortfall != null
					? [{ label: "ES", color: t.status.serious }]
					: []),
			]}
		>
			{hist.length > 0 && (
				<Cartesian
					height={260}
					xDomain={niceDomain(
						hist.flatMap((h) => [h.x0, h.x1]),
						{
							includeZero: true,
						},
					)}
					yDomain={[0, Math.max(...hist.map((h) => h.n)) * 1.05]}
					xLabel="P&L"
					zeroLine
					xFormat={(v) => v.toFixed(0)}
					yFormat={(v) => v.toFixed(0)}
				>
					{({ xScale, yScale, innerHeight }) => (
						<>
							{hist.map((h, i) => (
								<Bar
									key={i}
									x={xScale(h.x0) + 0.5}
									width={Math.max(0, xScale(h.x1) - xScale(h.x0) - 1)}
									y={yScale(h.n)}
									height={innerHeight - yScale(h.n)}
									fill={t.series[0]}
									opacity={0.75}
								/>
							))}
							{varLevel != null && (
								<line
									x1={xScale(varLevel)}
									x2={xScale(varLevel)}
									y1={0}
									y2={innerHeight}
									stroke={t.status.critical}
									strokeWidth={1.5}
								/>
							)}
							{expectedShortfall != null && (
								<line
									x1={xScale(expectedShortfall)}
									x2={xScale(expectedShortfall)}
									y1={0}
									y2={innerHeight}
									stroke={t.status.serious}
									strokeWidth={1.5}
									strokeDasharray="3 2"
								/>
							)}
						</>
					)}
				</Cartesian>
			)}
		</ChartFrame>
	);
}
