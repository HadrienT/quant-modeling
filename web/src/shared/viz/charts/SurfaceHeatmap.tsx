import { sequentialColor } from "@/shared/styles/tokens";
import { ChartFrame } from "../ChartFrame";
import { useChartTheme } from "../theme";

export type Grid = {
	x: number[];
	y: number[];
	z: (number | null)[][]; // z[yi][xi]
	xLabel?: string;
	yLabel?: string;
};

/**
 * Sequential single-hue heatmap; holes rendered AS holes, never interpolated
 * (blueprint WP 06 §2, dependencies §3.5). Doubles as the 2D fallback for the
 * WebGL surface (WP 05 §7).
 */
export function SurfaceHeatmap({
	grid,
	title = "Surface",
	isLoading,
	error,
}: {
	grid?: Grid;
	title?: string;
	isLoading?: boolean;
	error?: unknown;
}) {
	const t = useChartTheme();
	const isEmpty = !grid || grid.z.length === 0;

	const flat = grid?.z.flat().filter((v): v is number => v != null) ?? [];
	const min = Math.min(...flat);
	const max = Math.max(...flat);
	const norm = (v: number) => (max === min ? 0.5 : (v - min) / (max - min));

	const cols = grid?.x.length ?? 0;
	const rows = grid?.y.length ?? 0;

	return (
		<ChartFrame
			title={title}
			subtitle={grid ? `${min.toFixed(3)} – ${max.toFixed(3)}` : undefined}
			isLoading={isLoading}
			error={error}
			isEmpty={isEmpty}
			height={280}
			table={
				grid
					? {
							columns: [grid.yLabel ?? "y \\ x", ...grid.x.map(String)],
							rows: grid.y.map((yv, yi) => [
								String(yv),
								...grid.x.map((_, xi) => {
									const v = grid.z[yi]?.[xi];
									return v == null ? "—" : v.toFixed(4);
								}),
							]),
						}
					: undefined
			}
		>
			{grid && (
				<div className="overflow-auto">
					<div
						className="grid gap-px"
						style={{
							gridTemplateColumns: `repeat(${cols}, minmax(14px, 1fr))`,
							minWidth: cols * 16,
						}}
					>
						{Array.from({ length: rows }).flatMap((_, yi) =>
							Array.from({ length: cols }).map((_, xi) => {
								const v = grid.z[yi]?.[xi];
								const hole = v == null;
								return (
									<div
										key={`${yi}-${xi}`}
										title={
											hole
												? `${grid.x[xi]} / ${grid.y[yi]}: no quote`
												: `${grid.x[xi]} / ${grid.y[yi]}: ${v!.toFixed(4)}`
										}
										className="aspect-square"
										style={{
											background: hole
												? `repeating-linear-gradient(45deg, ${t.hairline}, ${t.hairline} 2px, transparent 2px, transparent 4px)`
												: sequentialColor(norm(v!), t),
										}}
									/>
								);
							}),
						)}
					</div>
				</div>
			)}
		</ChartFrame>
	);
}
