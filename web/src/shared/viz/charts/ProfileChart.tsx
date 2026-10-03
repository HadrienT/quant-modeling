import { Area, LinePath } from "@visx/shape";
import { curveLinear } from "@visx/curve";
import { Cartesian } from "../Cartesian";
import { ChartFrame } from "../ChartFrame";
import { niceDomain } from "../format";
import { useChartTheme } from "../theme";

export type ProfileLine = {
	label: string;
	values: number[];
	/** A reference drawn dashed: the same thing under another assumption. */
	dashed?: boolean;
};

export type ProfileBand = {
	label: string;
	lower: number[];
	upper: number[];
};

const COMPACT = new Intl.NumberFormat("en-US", {
	notation: "compact",
	maximumFractionDigits: 1,
});
const money = (v: number) => (v < 0 ? "−" : "") + COMPACT.format(Math.abs(v));
const years = (v: number) => `${Math.round(v * 100) / 100}y`;

/**
 * Profiles through time on a common grid of dates: lines (an expected
 * exposure, a quantile) over optional bands (each the range between two
 * quantiles; given from the widest to the narrowest they stack into a fan,
 * darker where the distribution is denser). Straight segments: the values exist at the dates and nowhere
 * else. The table lists every date of every line.
 */
export function ProfileChart({
	times,
	lines,
	bands,
	bandsLabel,
	title,
	yLabel,
	isLoading,
	error,
	height = 300,
}: {
	times?: number[];
	lines?: ProfileLine[];
	bands?: ProfileBand[];
	/** One legend entry for the whole fan, instead of one per band. */
	bandsLabel?: string;
	title?: string;
	yLabel?: string;
	isLoading?: boolean;
	error?: unknown;
	height?: number;
}) {
	const t = useChartTheme();
	const all = [
		...(lines?.flatMap((l) => l.values) ?? []),
		...(bands?.flatMap((b) => [...b.lower, ...b.upper]) ?? []),
	];
	const isEmpty = !times?.length || all.length === 0;
	const index = times?.map((_, i) => i) ?? [];

	return (
		<ChartFrame
			title={title}
			isLoading={isLoading}
			error={error}
			isEmpty={isEmpty}
			height={height}
			legend={[
				...(lines?.map((l, i) => ({
					label: l.label,
					color: t.series[i % t.series.length]!,
				})) ?? []),
				...(bands?.length
					? (bandsLabel ? [bandsLabel] : bands.map((b) => b.label)).map(
							(label) => ({ label, color: t.inkMuted }),
						)
					: []),
			]}
			table={
				times && lines?.length
					? {
							columns: [
								"Date",
								...lines.map((l) => l.label),
								...(bands?.flatMap((b) => [
									`${b.label}, low`,
									`${b.label}, high`,
								]) ?? []),
							],
							rows: times.map((x, i) => [
								years(x),
								...lines.map((l) => Math.round(l.values[i] ?? 0)),
								...(bands?.flatMap((b) => [
									Math.round(b.lower[i] ?? 0),
									Math.round(b.upper[i] ?? 0),
								]) ?? []),
							]),
						}
					: undefined
			}
		>
			{times && lines && (
				<Cartesian
					height={height}
					margin={{ top: 12, right: 16, bottom: 28, left: 56 }}
					xDomain={[0, times[times.length - 1] ?? 1]}
					yDomain={niceDomain(all)}
					xLabel="Years"
					yLabel={yLabel}
					xFormat={years}
					yFormat={money}
					zeroLine
				>
					{({ xScale, yScale }) => (
						<>
							{bands?.map((b) => (
								<Area
									key={b.label}
									data={index}
									x={(i) => xScale(times[i]!)}
									y0={(i) => yScale(b.lower[i]!)}
									y1={(i) => yScale(b.upper[i]!)}
									fill={t.inkMuted}
									fillOpacity={0.13}
									curve={curveLinear}
								/>
							))}
							{lines.map((l, k) => (
								<LinePath
									key={l.label}
									data={index}
									x={(i) => xScale(times[i]!)}
									y={(i) => yScale(l.values[i]!)}
									stroke={t.series[k % t.series.length]}
									strokeWidth={1.75}
									strokeDasharray={l.dashed ? "5 4" : undefined}
									curve={curveLinear}
								/>
							))}
						</>
					)}
				</Cartesian>
			)}
		</ChartFrame>
	);
}
