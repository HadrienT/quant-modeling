import { LinePath } from "@visx/shape";
import { curveMonotoneX } from "@visx/curve";
import { Cartesian } from "../Cartesian";
import { ChartFrame } from "../ChartFrame";
import { niceDomain } from "../format";
import { useChartTheme } from "../theme";

export type GreekProfile = { name: string; spot: number[]; values: number[] };

/**
 * One greek vs spot. Five greeks → five small multiples, never five lines on one
 * axis (blueprint WP 06 §2, pitfalls). Zero visible (greeks are signed).
 */
export function GreekProfileChart({
	profiles,
	currentSpot,
	title = "Greek profiles",
	isLoading,
	error,
}: {
	profiles?: GreekProfile[];
	currentSpot?: number;
	title?: string;
	isLoading?: boolean;
	error?: unknown;
}) {
	const t = useChartTheme();
	const isEmpty = !profiles || profiles.length === 0;

	return (
		<ChartFrame
			title={title}
			isLoading={isLoading}
			error={error}
			isEmpty={isEmpty}
			height={profiles && profiles.length > 1 ? 360 : 200}
			table={
				profiles?.[0]
					? {
							columns: ["Spot", ...profiles.map((p) => p.name)],
							rows: profiles[0].spot.map((s, i) => [
								s.toFixed(1),
								...profiles.map((p) => (p.values[i] ?? 0).toPrecision(4)),
							]),
						}
					: undefined
			}
		>
			{profiles && (
				<div className="grid gap-3 sm:grid-cols-2 xl:grid-cols-3">
					{profiles.map((p) => (
						<div key={p.name} className="flex flex-col gap-1">
							<span className="text-2xs font-medium text-ink-secondary uppercase">
								{p.name}
							</span>
							<Cartesian
								height={150}
								margin={{ top: 6, right: 8, bottom: 20, left: 40 }}
								xDomain={niceDomain(p.spot)}
								yDomain={niceDomain(p.values, { includeZero: true })}
								numXTicks={4}
								numYTicks={4}
								zeroLine
								xFormat={(v) => v.toFixed(0)}
								yFormat={(v) =>
									Math.abs(v) < 0.01 ? v.toExponential(0) : v.toFixed(2)
								}
							>
								{({ xScale, yScale, innerHeight }) => (
									<>
										<LinePath
											data={p.spot.map((s, i) => [s, p.values[i] ?? 0])}
											x={(d) => xScale(d[0]!)}
											y={(d) => yScale(d[1]!)}
											stroke={t.series[0]}
											strokeWidth={1.5}
											curve={curveMonotoneX}
										/>
										{currentSpot != null && (
											<line
												x1={xScale(currentSpot)}
												x2={xScale(currentSpot)}
												y1={0}
												y2={innerHeight}
												stroke={t.accent}
												strokeWidth={1}
											/>
										)}
									</>
								)}
							</Cartesian>
						</div>
					))}
				</div>
			)}
		</ChartFrame>
	);
}
