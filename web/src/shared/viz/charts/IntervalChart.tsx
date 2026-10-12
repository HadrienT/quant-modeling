import { Cartesian } from "../Cartesian";
import { ChartFrame } from "../ChartFrame";
import { niceDomain } from "../format";
import { useChartTheme } from "../theme";

export type IntervalRow = {
	label: string;
	value: number | null;
	lower: number | null;
	upper: number | null;
};

/**
 * Estimates with their confidence intervals, one per line: a dot for the
 * value, a bar from the lower to the upper bound. Two estimates whose bars
 * overlap widely are not told apart by the data. A row without a value is
 * listed and left empty.
 */
export function IntervalChart({
	rows,
	format,
	title,
	subtitle,
	xLabel,
	reference = 0,
}: {
	rows: IntervalRow[];
	format: (v: number) => string;
	title?: string;
	subtitle?: string;
	xLabel?: string;
	/** A vertical line to read the intervals against (default: zero). */
	reference?: number | null;
}) {
	const t = useChartTheme();
	const height = 72 + rows.length * 30;
	const all = rows.flatMap((r) =>
		[r.value, r.lower, r.upper].filter((v): v is number => v != null),
	);
	const cell = (v: number | null) => (v == null ? "" : format(v));

	return (
		<ChartFrame
			title={title}
			subtitle={subtitle}
			isEmpty={all.length === 0}
			height={height}
			table={{
				columns: ["", "Value", "Lower", "Upper"],
				rows: rows.map((r) => [
					r.label,
					cell(r.value),
					cell(r.lower),
					cell(r.upper),
				]),
			}}
		>
			<Cartesian
				height={height}
				margin={{ top: 8, right: 16, bottom: 44, left: 112 }}
				xDomain={niceDomain(reference == null ? all : [...all, reference])}
				yDomain={[rows.length - 0.5, -0.5]}
				numYTicks={rows.length}
				xFormat={format}
				yFormat={(i) => rows[Math.round(i)]?.label ?? ""}
				xLabel={xLabel}
			>
				{({ xScale, yScale, innerHeight }) => (
					<>
						{reference != null && (
							<line
								x1={xScale(reference)}
								x2={xScale(reference)}
								y1={0}
								y2={innerHeight}
								stroke={t.axis}
								strokeWidth={1}
							/>
						)}
						{rows.map((r, i) => {
							const stroke = t.series[i % t.series.length];
							return (
								<g key={r.label}>
									{r.lower != null && r.upper != null && (
										<line
											x1={xScale(r.lower)}
											x2={xScale(r.upper)}
											y1={yScale(i)}
											y2={yScale(i)}
											stroke={stroke}
											strokeWidth={3}
											strokeLinecap="round"
											opacity={0.55}
										/>
									)}
									{r.value != null && (
										<circle
											cx={xScale(r.value)}
											cy={yScale(i)}
											r={4.5}
											fill={stroke}
											stroke={t.surface}
											strokeWidth={1.5}
										/>
									)}
								</g>
							);
						})}
					</>
				)}
			</Cartesian>
		</ChartFrame>
	);
}
