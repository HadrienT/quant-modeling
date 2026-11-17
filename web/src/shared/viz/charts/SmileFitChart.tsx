import { Area, Circle, LinePath } from "@visx/shape";
import { Cartesian } from "../Cartesian";
import { ChartFrame } from "../ChartFrame";
import { niceDomain } from "../format";
import { useChartTheme } from "../theme";

export type SmileFitSeries = {
	label: string;
	x: number[];
	iv: (number | null)[];
	/** Standard errors: drawn as a 95% band (iv ± 1.96 se). */
	se?: (number | null)[];
	color?: string;
	dashed?: boolean;
};

type Pt = { x: number; y: number; se: number | null };

/** Runs of consecutive defined points: a hole (null) breaks the line. */
function runs(s: SmileFitSeries): Pt[][] {
	const out: Pt[][] = [];
	let cur: Pt[] = [];
	s.x.forEach((x, j) => {
		const y = s.iv[j];
		if (y == null || !Number.isFinite(y)) {
			if (cur.length) out.push(cur);
			cur = [];
		} else cur.push({ x, y, se: s.se?.[j] ?? null });
	});
	if (cur.length) out.push(cur);
	return out;
}

/**
 * Implied-vol smiles with their Monte Carlo error (WP 29): a model's smile as
 * a line with a 95% band and markers, against another (the market's) as a
 * plain line. Implied vols in decimals, shown in percent.
 */
export function SmileFitChart({
	series = [],
	xLabel = "K / F",
	xFormat = (v) => v.toFixed(2),
	height = 280,
	title,
	subtitle,
	isLoading,
	error,
}: {
	series?: SmileFitSeries[];
	xLabel?: string;
	xFormat?: (v: number) => string;
	height?: number;
	title?: string;
	subtitle?: string;
	isLoading?: boolean;
	error?: unknown;
}) {
	const t = useChartTheme();
	const all = series.flatMap(runs).flat();
	const ys = all.flatMap((p) =>
		p.se != null ? [p.y - 1.96 * p.se, p.y + 1.96 * p.se] : [p.y],
	);
	const color = (i: number, s: SmileFitSeries) =>
		s.color ?? t.series[i % t.series.length]!;

	return (
		<ChartFrame
			title={title}
			subtitle={subtitle}
			isLoading={isLoading}
			error={error}
			isEmpty={all.length === 0}
			height={height}
			legend={series.map((s, i) => ({ label: s.label, color: color(i, s) }))}
		>
			<Cartesian
				height={height}
				xDomain={niceDomain(all.map((p) => p.x))}
				yDomain={niceDomain(ys)}
				xLabel={xLabel}
				xFormat={xFormat}
				yFormat={(v) => `${(v * 100).toFixed(1)}%`}
				margin={{ top: 12, right: 28, bottom: 34, left: 52 }}
			>
				{({ xScale, yScale }) =>
					series.map((s, i) => {
						const c = color(i, s);
						return (
							<g key={s.label}>
								{runs(s).map((run, k) => (
									<g key={k}>
										{s.se && (
											<Area
												data={run.filter((p) => p.se != null)}
												x={(p) => xScale(p.x)}
												y0={(p) => yScale(p.y - 1.96 * p.se!)}
												y1={(p) => yScale(p.y + 1.96 * p.se!)}
												fill={c}
												fillOpacity={0.18}
											/>
										)}
										<LinePath
											data={run}
											x={(p) => xScale(p.x)}
											y={(p) => yScale(p.y)}
											stroke={c}
											strokeWidth={2}
											strokeDasharray={s.dashed ? "5 4" : undefined}
										/>
										{s.se &&
											run.map((p) => (
												<Circle
													key={p.x}
													cx={xScale(p.x)}
													cy={yScale(p.y)}
													r={2.5}
													fill={c}
												/>
											))}
									</g>
								))}
							</g>
						);
					})
				}
			</Cartesian>
		</ChartFrame>
	);
}
