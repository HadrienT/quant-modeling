import type { MouseEvent } from "react";
import { Area, Line, LinePath } from "@visx/shape";
import { Cartesian } from "../Cartesian";
import { ChartFrame, type LegendItem } from "../ChartFrame";
import { niceDomain } from "../format";
import { useChartTheme } from "../theme";

type Series = (number | null)[];
export type FanBand = { label: string; lo: Series; hi: Series };
export type FanLine = {
	label: string;
	y: Series;
	color?: string;
	dashed?: boolean;
};

const finite = (v: number | null | undefined): v is number =>
	v != null && Number.isFinite(v);

/**
 * A fan chart over time (WP 29): quantile bands (outermost first) shaded
 * from light to dark, a few sample paths over them, named lines on top, and
 * a cursor on one date that the caller moves (`onCursor`, on hover or
 * click). `revealCount` truncates everything to its first N + 1 points: the
 * caller animates "the simulation drawing itself" with it. Holes (null) are
 * gaps, never interpolated.
 */
export function FanChart({
	time,
	bands = [],
	paths = [],
	lines = [],
	revealCount,
	cursor,
	onCursor,
	yFormat = (v) => v.toFixed(1),
	height = 320,
	title,
	subtitle,
	isLoading,
	error,
}: {
	time: number[];
	bands?: FanBand[];
	paths?: Series[];
	lines?: FanLine[];
	revealCount?: number;
	cursor?: number;
	onCursor?: (index: number) => void;
	yFormat?: (v: number) => string;
	height?: number;
	title?: string;
	subtitle?: string;
	isLoading?: boolean;
	error?: unknown;
}) {
	const t = useChartTheme();
	const n = revealCount ?? time.length - 1;
	const cut = (s: Series) => s.slice(0, n + 1);
	const values = [
		...bands.flatMap((b) => [...b.lo, ...b.hi]),
		...paths.flat(),
		...lines.flatMap((l) => l.y),
	].filter(finite);
	const tMax = time[time.length - 1] ?? 1;
	const pathColor = t.series[0]!;
	const legend: LegendItem[] = [
		...bands.map((b, i) => ({
			label: b.label,
			color: t.sequential[Math.min(i + 1, t.sequential.length - 1)]!,
		})),
		...(paths.length ? [{ label: "Paths", color: pathColor }] : []),
		...lines.map((l, i) => ({
			label: l.label,
			color: l.color ?? t.series[(i + 1) % t.series.length]!,
		})),
	];

	const pick = (e: MouseEvent<SVGRectElement>, width: number) => {
		if (!onCursor || time.length < 2) return;
		const r = e.currentTarget.getBoundingClientRect();
		const at = ((e.clientX - r.left) / Math.max(width, 1)) * tMax;
		const i = Math.round((at / tMax) * (time.length - 1));
		onCursor(Math.min(Math.max(i, 1), time.length - 1));
	};

	return (
		<ChartFrame
			title={title}
			subtitle={subtitle}
			isLoading={isLoading}
			error={error}
			isEmpty={values.length === 0}
			height={height}
			legend={legend}
		>
			<Cartesian
				height={height}
				xDomain={[0, tMax]}
				yDomain={niceDomain(values)}
				xLabel="Years"
				xFormat={(v) => v.toFixed(2)}
				yFormat={yFormat}
				margin={{ top: 12, right: 28, bottom: 34, left: 56 }}
			>
				{({ xScale, yScale, innerWidth, innerHeight }) => {
					const pts = (s: Series) =>
						cut(s)
							.map((v, i) => [time[i]!, v] as const)
							.filter((d): d is readonly [number, number] => finite(d[1]));
					return (
						<>
							{bands.map((b, k) => {
								const data = cut(b.lo)
									.map((lo, i) => ({ t: time[i]!, lo, hi: b.hi[i] }))
									.filter((d) => finite(d.lo) && finite(d.hi));
								return (
									<Area
										key={b.label}
										data={data}
										x={(d) => xScale(d.t)}
										y0={(d) => yScale(d.lo!)}
										y1={(d) => yScale(d.hi!)}
										fill={
											t.sequential[Math.min(k + 1, t.sequential.length - 1)]
										}
										fillOpacity={0.35}
									/>
								);
							})}
							{paths.map((p, k) => (
								<LinePath
									key={k}
									data={pts(p)}
									x={(d) => xScale(d[0])}
									y={(d) => yScale(d[1])}
									stroke={pathColor}
									strokeWidth={1}
									strokeOpacity={0.35}
								/>
							))}
							{lines.map((l, i) => (
								<LinePath
									key={l.label}
									data={pts(l.y)}
									x={(d) => xScale(d[0])}
									y={(d) => yScale(d[1])}
									stroke={l.color ?? t.series[(i + 1) % t.series.length]}
									strokeWidth={2}
									strokeDasharray={l.dashed ? "5 4" : undefined}
								/>
							))}
							{cursor != null && time[cursor] != null && (
								<Line
									from={{ x: xScale(time[cursor]!), y: 0 }}
									to={{ x: xScale(time[cursor]!), y: innerHeight }}
									stroke={t.ink}
									strokeDasharray="3 3"
									strokeOpacity={0.7}
								/>
							)}
							{onCursor && (
								<rect
									width={innerWidth}
									height={innerHeight}
									fill="transparent"
									style={{ cursor: "crosshair" }}
									onClick={(e) => pick(e, innerWidth)}
									onMouseMove={(e) => e.buttons === 1 && pick(e, innerWidth)}
								/>
							)}
						</>
					);
				}}
			</Cartesian>
		</ChartFrame>
	);
}
