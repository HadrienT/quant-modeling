import { Suspense, lazy, useMemo, useState } from "react";
import { Box, Camera, Grid3x3, Table2 } from "lucide-react";
import { Button, cn } from "@/shared/ui";
import { ChartSkeleton } from "@/shared/ui/states";
import { SurfaceHeatmap } from "../charts/SurfaceHeatmap";
import { type SurfaceGrid, holeFraction, zAt } from "./SurfaceGrid";
import type { PresetView } from "./SurfaceScene";

const SurfaceScene = lazy(() =>
	import("./SurfaceScene").then((m) => ({ default: m.SurfaceScene })),
);

/**
 * WebGL surface with mandatory graceful degradation (blueprint WP 05 §7):
 *  - no WebGL2 / lost context → the 2D heatmap, with a reason
 *  - prefers-reduced-motion → no camera transitions or auto-rotate
 *  - screen reader / keyboard only → the table view (always reachable, not
 *    hidden behind a preference)
 */
export function SurfaceView({
	grid,
	mode = "sequential",
	title,
	height = 420,
}: {
	grid?: SurfaceGrid;
	mode?: "sequential" | "divergent";
	title?: string;
	height?: number;
}) {
	const webgl = useMemo(hasWebGL2, []);
	const reducedMotion = useMemo(
		() =>
			typeof window !== "undefined" &&
			window.matchMedia?.("(prefers-reduced-motion: reduce)").matches,
		[],
	);

	const [view, setView] = useState<PresetView>("three-quarter");
	const [showGrid, setShowGrid] = useState(false);
	const [showTable, setShowTable] = useState(false);

	if (!grid) return <ChartSkeleton className="!aspect-auto" />;

	const heatmapGrid = {
		x: Array.from(grid.x),
		y: Array.from(grid.y),
		z: Array.from(grid.y, (_, yi) =>
			Array.from(grid.x, (_, xi) => {
				const v = zAt(grid, xi, yi);
				return Number.isNaN(v) ? null : v;
			}),
		),
		xLabel: grid.axes.x.label,
		yLabel: grid.axes.y.label,
	};

	if (!webgl) {
		return (
			<div className="flex flex-col gap-2">
				<p className="text-2xs text-warning">
					WebGL2 is unavailable — showing the 2D heatmap instead.
				</p>
				<SurfaceHeatmap grid={heatmapGrid} title={title} />
			</div>
		);
	}

	return (
		<figure className="flex flex-col gap-2 rounded-md border border-hairline bg-surface p-3">
			<div className="flex items-center justify-between gap-2">
				<figcaption className="text-sm font-medium text-ink">
					{title}
					<span className="ml-2 text-2xs text-ink-muted">
						{(holeFraction(grid) * 100).toFixed(0)}% holes shown as absence
					</span>
				</figcaption>
				<div className="flex items-center gap-1">
					{(
						[
							["three-quarter", "3/4"],
							["front", "Smile"],
							["profile", "Term"],
							["top", "Top"],
						] as [PresetView, string][]
					).map(([v, label]) => (
						<Button
							key={v}
							size="sm"
							variant="ghost"
							aria-pressed={view === v}
							className={cn("text-2xs", view === v && "text-ink")}
							onClick={() => setView(v)}
						>
							{label}
						</Button>
					))}
					<Button
						size="icon"
						variant="ghost"
						aria-pressed={showGrid}
						aria-label="Toggle data grid overlay"
						onClick={() => setShowGrid((v) => !v)}
					>
						<Grid3x3 className="size-3.5" />
					</Button>
					<Button
						size="icon"
						variant="ghost"
						aria-pressed={showTable}
						aria-label="Toggle table view"
						onClick={() => setShowTable((v) => !v)}
					>
						<Table2 className="size-3.5" />
					</Button>
					<Button
						size="icon"
						variant="ghost"
						aria-label="Export PNG"
						onClick={() => {
							import("./SurfaceScene").then((m) => {
								const url = m.exportSceneToPng();
								if (!url) return;
								const a = document.createElement("a");
								a.href = url;
								a.download = `${title ?? "surface"}.png`;
								a.click();
							});
						}}
					>
						<Camera className="size-3.5" />
					</Button>
				</div>
			</div>

			{showTable ? (
				<SurfaceHeatmap grid={heatmapGrid} />
			) : (
				<div style={{ height }}>
					<Suspense
						fallback={
							<div className="flex h-full items-center justify-center text-xs text-ink-muted">
								<Box className="mr-2 size-4 animate-pulse" />
								loading 3D engine…
							</div>
						}
					>
						<SurfaceScene
							grid={grid}
							mode={mode}
							view={view}
							showDataGrid={showGrid}
							reducedMotion={reducedMotion}
						/>
					</Suspense>
				</div>
			)}
		</figure>
	);
}

function hasWebGL2(): boolean {
	if (typeof document === "undefined") return false;
	try {
		const canvas = document.createElement("canvas");
		return !!canvas.getContext("webgl2");
	} catch {
		return false;
	}
}
