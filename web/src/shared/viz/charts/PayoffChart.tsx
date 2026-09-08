import { LinePath } from "@visx/shape";
import { curveMonotoneX } from "@visx/curve";
import { Cartesian } from "../Cartesian";
import { ChartFrame, type TableView } from "../ChartFrame";
import { niceDomain } from "../format";
import { useChartTheme } from "../theme";

export type PayoffSeries = {
	spot: number[];
	atMaturity: number[];
	atT?: number[];
	legs?: { label: string; values: number[] }[];
	breakevens?: number[];
	currentSpot?: number;
	strikes?: number[];
};

/**
 * Payoff at maturity (solid) and value at t (thinner), breakevens / strikes /
 * spot annotated, profit & loss zones in a faint blue↔red wash — never red/green
 * (blueprint WP 06 §2, WP 10 §3). Single y-axis, zero always visible.
 */
export function PayoffChart({
	data,
	title = "Payoff",
	isLoading,
	error,
	height = 300,
	hoveredLeg,
	onLegHover,
}: {
	data?: PayoffSeries;
	title?: string;
	isLoading?: boolean;
	error?: unknown;
	height?: number;
	/** Index of the leg whose dashed line should be emphasised. */
	hoveredLeg?: number | null;
	/** Reports which per-leg line the pointer is over (null on leave). */
	onLegHover?: (index: number | null) => void;
}) {
	const t = useChartTheme();
	const isEmpty = !data || data.spot.length === 0;

	const table: TableView | undefined = data
		? {
				columns: ["Spot", "Payoff @T", ...(data.atT ? ["Value @t"] : [])],
				rows: data.spot.map((s, i) => [
					s.toFixed(2),
					(data.atMaturity[i] ?? 0).toFixed(2),
					...(data.atT ? [(data.atT[i] ?? 0).toFixed(2)] : []),
				]),
			}
		: undefined;

	return (
		<ChartFrame
			title={title}
			legend={[
				{ label: "Payoff at maturity", color: t.series[0]! },
				...(data?.atT ? [{ label: "Value at t", color: t.inkSecondary }] : []),
			]}
			table={table}
			isLoading={isLoading}
			error={error}
			isEmpty={isEmpty}
			height={height}
		>
			{data && (
				<Cartesian
					height={height}
					xDomain={niceDomain(data.spot)}
					yDomain={niceDomain([...data.atMaturity, ...(data.atT ?? [])], {
						includeZero: true,
					})}
					xLabel="Underlying"
					zeroLine
					xFormat={(v) => v.toFixed(0)}
					yFormat={(v) => v.toFixed(1)}
				>
					{({ xScale, yScale, innerWidth, innerHeight }) => (
						<>
							{/* profit / loss wash */}
							<defs>
								<clipPath id="payoff-profit">
									<rect x={0} y={0} width={innerWidth} height={yScale(0)} />
								</clipPath>
							</defs>
							<rect
								x={0}
								y={0}
								width={innerWidth}
								height={Math.max(0, yScale(0))}
								fill={t.divergent[1]}
								opacity={0.05}
							/>
							<rect
								x={0}
								y={Math.max(0, yScale(0))}
								width={innerWidth}
								height={Math.max(0, innerHeight - yScale(0))}
								fill={t.divergent[0]}
								opacity={0.05}
							/>

							{data.legs?.map((leg, li) => {
								const pts = data.spot.map(
									(s, i) => [s, leg.values[i] ?? 0] as [number, number],
								);
								const active = hoveredLeg === li;
								return (
									<g key={li}>
										<LinePath
											data={pts}
											x={(d) => xScale(d[0]!)}
											y={(d) => yScale(d[1]!)}
											stroke={active ? t.series[1]! : t.inkMuted}
											strokeWidth={active ? 1.75 : 0.75}
											{...(active ? {} : { strokeDasharray: "2 2" })}
										/>
										{onLegHover && (
											<LinePath
												data={pts}
												x={(d) => xScale(d[0]!)}
												y={(d) => yScale(d[1]!)}
												stroke="transparent"
												strokeWidth={12}
												pointerEvents="stroke"
												style={{ cursor: "pointer" }}
												onMouseEnter={() => onLegHover(li)}
												onMouseLeave={() => onLegHover(null)}
											/>
										)}
									</g>
								);
							})}

							{data.atT && (
								<LinePath
									data={data.spot.map((s, i) => [s, data.atT![i] ?? 0])}
									x={(d) => xScale(d[0]!)}
									y={(d) => yScale(d[1]!)}
									stroke={t.inkSecondary}
									strokeWidth={1.25}
									curve={curveMonotoneX}
								/>
							)}

							<LinePath
								data={data.spot.map((s, i) => [s, data.atMaturity[i] ?? 0])}
								x={(d) => xScale(d[0]!)}
								y={(d) => yScale(d[1]!)}
								stroke={t.series[0]}
								strokeWidth={2}
							/>

							{data.strikes?.map((k) => (
								<line
									key={`k-${k}`}
									x1={xScale(k)}
									x2={xScale(k)}
									y1={0}
									y2={innerHeight}
									stroke={t.axis}
									strokeWidth={1}
									strokeDasharray="3 3"
								/>
							))}
							{data.breakevens?.map((b) => (
								<circle
									key={`be-${b}`}
									cx={xScale(b)}
									cy={yScale(0)}
									r={3}
									fill={t.ink}
								/>
							))}
							{data.currentSpot != null && (
								<line
									x1={xScale(data.currentSpot)}
									x2={xScale(data.currentSpot)}
									y1={0}
									y2={innerHeight}
									stroke={t.accent}
									strokeWidth={1.25}
								/>
							)}
						</>
					)}
				</Cartesian>
			)}
		</ChartFrame>
	);
}
