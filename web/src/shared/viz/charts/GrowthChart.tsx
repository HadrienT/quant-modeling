import { LinePath } from "@visx/shape";
import { curveLinear } from "@visx/curve";
import { Cartesian } from "../Cartesian";
import { ChartFrame } from "../ChartFrame";
import { niceDomain, tickDate, yearFraction } from "../format";
import { useChartTheme } from "../theme";

export type GrowthSeries = {
	label: string;
	/** One value per date; null where the series does not exist. */
	values: (number | null)[];
	/** A reference drawn dashed: a comparison point, not the subject. */
	dashed?: boolean;
};

const COMPACT = new Intl.NumberFormat("en-US", {
	notation: "compact",
	maximumFractionDigits: 2,
});
/**
 * Values through calendar time on common dates: the subject and its
 * comparison points, on a linear scale. `marks` are dates drawn as a rug
 * under the curves (the days a decision was taken). The table lists the
 * values at the marks when there are any, otherwise at every date.
 */
export function GrowthChart({
	dates,
	series,
	marks,
	marksLabel,
	title,
	subtitle,
	yLabel,
	height = 340,
}: {
	dates: string[];
	series: GrowthSeries[];
	marks?: string[];
	marksLabel?: string;
	title?: string;
	subtitle?: string;
	yLabel?: string;
	height?: number;
}) {
	const t = useChartTheme();
	const x = dates.map(yearFraction);
	const all = series.flatMap((s) =>
		s.values.filter((v): v is number => v != null),
	);
	const rowOf = new Map(dates.map((d, i) => [d, i]));
	const listed = marks?.length
		? marks.map((d) => rowOf.get(d)).filter((i): i is number => i != null)
		: dates.map((_, i) => i);

	return (
		<ChartFrame
			title={title}
			subtitle={subtitle}
			isEmpty={dates.length === 0 || all.length === 0}
			height={height}
			legend={[
				...series.map((s, i) => ({
					label: s.label,
					color: t.series[i % t.series.length]!,
				})),
				...(marks?.length && marksLabel
					? [{ label: marksLabel, color: t.inkMuted }]
					: []),
			]}
			table={{
				columns: ["Date", ...series.map((s) => s.label)],
				rows: listed.map((i) => [
					dates[i]!,
					...series.map((s) =>
						s.values[i] == null ? "" : Math.round(s.values[i]!),
					),
				]),
			}}
		>
			<Cartesian
				height={height}
				margin={{ top: 12, right: 16, bottom: 28, left: 56 }}
				xDomain={[x[0] ?? 0, x[x.length - 1] ?? 1]}
				yDomain={niceDomain(all)}
				yLabel={yLabel}
				xFormat={tickDate}
				yFormat={(v) => COMPACT.format(v)}
			>
				{({ xScale, yScale, innerHeight }) => (
					<>
						{marks?.map((d) => (
							<line
								key={d}
								x1={xScale(yearFraction(d))}
								x2={xScale(yearFraction(d))}
								y1={innerHeight - 6}
								y2={innerHeight}
								stroke={t.inkMuted}
								strokeWidth={1}
							/>
						))}
						{series.map((s, k) => (
							<LinePath
								key={s.label}
								data={x.map((_, i) => i).filter((i) => s.values[i] != null)}
								x={(i) => xScale(x[i]!)}
								y={(i) => yScale(s.values[i]!)}
								stroke={t.series[k % t.series.length]}
								strokeWidth={s.dashed ? 1.25 : 2}
								strokeDasharray={s.dashed ? "5 4" : undefined}
								curve={curveLinear}
							/>
						))}
					</>
				)}
			</Cartesian>
		</ChartFrame>
	);
}
