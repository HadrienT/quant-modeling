import { Bar, Line, LinePath } from "@visx/shape";
import { Cartesian } from "../Cartesian";
import { ChartFrame, type LegendItem } from "../ChartFrame";
import { niceDomain } from "../format";
import { useChartTheme } from "../theme";

export type DensityHistogram = {
	label: string;
	lo: number;
	hi: number;
	counts: number[];
	/** Every sample, inside the bins or not: the bars integrate to the share inside. */
	total: number;
	color?: string;
	/** Drawn as a step outline instead of filled bars (to overlay two). */
	outline?: boolean;
};
export type DensityCurve = {
	label: string;
	x: number[];
	y: (number | null)[];
	color?: string;
	dashed?: boolean;
};

/** Bars of height count / (total x width): a density, comparable with a pdf. */
function binDensities(h: DensityHistogram) {
	const width = (h.hi - h.lo) / Math.max(h.counts.length, 1);
	return h.counts.map((c, b) => ({
		x0: h.lo + b * width,
		x1: h.lo + (b + 1) * width,
		y: width > 0 ? c / (h.total * width) : 0,
	}));
}

/**
 * Densities of one quantity (WP 29): histograms of samples, normalised to
 * integrate to the share of samples they hold, against density curves (a
 * closed form, the market's). Optional vertical markers (a forward, a strike).
 */
export function DensityChart({
	histograms = [],
	curves = [],
	markers = [],
	xLabel,
	xFormat = (v) => v.toFixed(0),
	height = 260,
	title,
	subtitle,
	isLoading,
	error,
}: {
	histograms?: DensityHistogram[];
	curves?: DensityCurve[];
	markers?: { label: string; x: number }[];
	xLabel?: string;
	xFormat?: (v: number) => string;
	height?: number;
	title?: string;
	subtitle?: string;
	isLoading?: boolean;
	error?: unknown;
}) {
	const t = useChartTheme();
	const bins = histograms.map(binDensities);
	const xs = [
		...bins.flatMap((b) => b.flatMap((d) => [d.x0, d.x1])),
		...curves.flatMap((c) => c.x),
	];
	const ys = [
		0,
		...bins.flatMap((b) => b.map((d) => d.y)),
		...curves.flatMap((c) => c.y.filter((v): v is number => v != null)),
	];
	const color = (i: number, own?: string) =>
		own ?? t.series[i % t.series.length]!;
	const legend: LegendItem[] = [
		...histograms.map((h, i) => ({ label: h.label, color: color(i, h.color) })),
		...curves.map((c, i) => ({
			label: c.label,
			color: color(histograms.length + i, c.color),
		})),
	];
	const [x0, x1] = xs.length ? [Math.min(...xs), Math.max(...xs)] : [0, 1];

	return (
		<ChartFrame
			title={title}
			subtitle={subtitle}
			isLoading={isLoading}
			error={error}
			isEmpty={xs.length === 0}
			height={height}
			legend={legend}
		>
			<Cartesian
				height={height}
				xDomain={[x0, x1]}
				yDomain={[0, niceDomain(ys)[1]]}
				xLabel={xLabel}
				xFormat={xFormat}
				yFormat={(v) => v.toPrecision(2)}
				margin={{ top: 12, right: 28, bottom: 34, left: 56 }}
			>
				{({ xScale, yScale, innerHeight }) => (
					<>
						{bins.map((b, i) => {
							const h = histograms[i]!;
							const c = color(i, h.color);
							if (h.outline) {
								const steps = b.flatMap((d) => [
									[d.x0, d.y],
									[d.x1, d.y],
								]);
								return (
									<LinePath
										key={h.label}
										data={steps}
										x={(d) => xScale(d[0]!)}
										y={(d) => yScale(d[1]!)}
										stroke={c}
										strokeWidth={1.75}
									/>
								);
							}
							return (
								<g key={h.label}>
									{b.map((d, k) => (
										<Bar
											key={k}
											x={xScale(d.x0)}
											y={yScale(d.y)}
											width={Math.max(0, xScale(d.x1) - xScale(d.x0) - 1)}
											height={Math.max(0, innerHeight - yScale(d.y))}
											fill={c}
											fillOpacity={0.45}
										/>
									))}
								</g>
							);
						})}
						{curves.map((c, i) => (
							<LinePath
								key={c.label}
								data={c.x
									.map((x, k) => [x, c.y[k]] as const)
									.filter((d): d is readonly [number, number] => d[1] != null)}
								x={(d) => xScale(d[0])}
								y={(d) => yScale(d[1])}
								stroke={color(histograms.length + i, c.color)}
								strokeWidth={2}
								strokeDasharray={c.dashed ? "5 4" : undefined}
							/>
						))}
						{markers.map((m) => (
							<Line
								key={m.label}
								from={{ x: xScale(m.x), y: 0 }}
								to={{ x: xScale(m.x), y: innerHeight }}
								stroke={t.inkMuted}
								strokeDasharray="2 3"
							/>
						))}
					</>
				)}
			</Cartesian>
		</ChartFrame>
	);
}
