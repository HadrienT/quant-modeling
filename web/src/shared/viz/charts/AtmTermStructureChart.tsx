import { LinePath } from "@visx/shape";
import { curveMonotoneX } from "@visx/curve";
import { Cartesian } from "../Cartesian";
import { ChartFrame } from "../ChartFrame";
import { niceDomain } from "../format";
import { useChartTheme } from "../theme";

export type TermStructurePoint = { ttm: number; vol: number };

/**
 * ATM implied vol against maturity -- the term-structure line a desk reads
 * for contango/backwardation, distinct from (and much simpler than) a
 * single strike slice through the full surface.
 */
export function AtmTermStructureChart({
	points,
	title = "ATM term structure",
	isLoading,
	error,
}: {
	points?: TermStructurePoint[];
	title?: string;
	isLoading?: boolean;
	error?: unknown;
}) {
	const t = useChartTheme();
	const isEmpty = !points || points.length === 0;

	return (
		<ChartFrame
			title={title}
			subtitle="ATM vol vs. maturity"
			isLoading={isLoading}
			error={error}
			isEmpty={isEmpty}
			height={220}
			table={
				points
					? {
							columns: ["Maturity (y)", "ATM vol"],
							rows: points.map((p) => [
								p.ttm.toFixed(3),
								`${(p.vol * 100).toFixed(2)}%`,
							]),
						}
					: undefined
			}
		>
			{points && (
				<Cartesian
					height={220}
					xDomain={niceDomain(points.map((p) => p.ttm), { includeZero: true })}
					yDomain={niceDomain(points.map((p) => p.vol))}
					xLabel="Maturity (years)"
					yFormat={(v) => `${(v * 100).toFixed(0)}%`}
					xFormat={(v) => v.toFixed(1)}
				>
					{({ xScale, yScale }) => (
						<LinePath
							data={points}
							x={(p) => xScale(p.ttm)}
							y={(p) => yScale(p.vol)}
							stroke={t.series[0]}
							strokeWidth={1.75}
							curve={curveMonotoneX}
						/>
					)}
				</Cartesian>
			)}
		</ChartFrame>
	);
}
