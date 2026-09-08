import { divergentColor } from "@/shared/styles/tokens";
import { ChartFrame } from "../ChartFrame";
import { useChartTheme } from "../theme";

export type StressCell = { row: string; col: string; value: number };

/**
 * Spot × vol stress heatmap. Divergent blue↔red, neutral grey at zero
 * (blueprint WP 06 §2, WP 09 §5). Click a cell → per-position breakdown.
 */
export function StressMatrix({
	rows,
	cols,
	cells,
	onSelect,
	title = "Stress matrix",
	isLoading,
	error,
}: {
	rows?: string[];
	cols?: string[];
	cells?: StressCell[];
	onSelect?: (cell: StressCell) => void;
	title?: string;
	isLoading?: boolean;
	error?: unknown;
}) {
	const t = useChartTheme();
	const isEmpty = !cells || cells.length === 0;
	const lookup = new Map(cells?.map((c) => [`${c.row}|${c.col}`, c.value]));
	const absMax = Math.max(...(cells?.map((c) => Math.abs(c.value)) ?? [1]), 1);

	return (
		<ChartFrame
			title={title}
			subtitle="blue = gain · red = loss · grey = flat"
			isLoading={isLoading}
			error={error}
			isEmpty={isEmpty}
			height={40 + (rows?.length ?? 0) * 34}
			table={
				rows && cols
					? {
							columns: ["Spot \\ Vol", ...cols],
							rows: rows.map((r) => [
								r,
								...cols.map((c) => (lookup.get(`${r}|${c}`) ?? 0).toFixed(0)),
							]),
						}
					: undefined
			}
		>
			{rows && cols && (
				<div className="overflow-auto">
					<table className="border-separate border-spacing-1 text-2xs">
						<thead>
							<tr>
								<th />
								{cols.map((c) => (
									<th key={c} className="px-1 py-0.5 text-ink-muted">
										{c}
									</th>
								))}
							</tr>
						</thead>
						<tbody>
							{rows.map((r) => (
								<tr key={r}>
									<th className="pr-2 text-right text-ink-muted">{r}</th>
									{cols.map((c) => {
										const v = lookup.get(`${r}|${c}`) ?? 0;
										return (
											<td key={c}>
												<button
													type="button"
													onClick={() =>
														onSelect?.({ row: r, col: c, value: v })
													}
													className="h-8 w-14 rounded-xs font-mono text-ink tabular-nums"
													style={{
														background: divergentColor(v / absMax, t),
													}}
												>
													{v >= 0 ? "+" : "−"}
													{Math.abs(v).toFixed(0)}
												</button>
											</td>
										);
									})}
								</tr>
							))}
						</tbody>
					</table>
				</div>
			)}
		</ChartFrame>
	);
}
