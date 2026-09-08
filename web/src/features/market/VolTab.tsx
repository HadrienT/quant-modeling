import { useMemo, useState } from "react";
import { AlertTriangle } from "lucide-react";
import {
	useCleanedIvSurface,
	useIvSurface,
	useLocalVolSurface,
} from "@/shared/api";
import { Badge, Metric, MetricRow } from "@/shared/ui";
import { ErrorState } from "@/shared/ui/states";
import {
	SmileChart,
	SurfaceView,
	checkArbitrage,
	coverage,
	differenceGrid,
	ivSurfaceToGrid,
	cleanedIvSurfaceToGrid,
	localVolSurfaceToGrid,
	zAt,
	holeFraction,
} from "@/shared/viz";

type Stage = "raw" | "cleaned" | "localvol";

/**
 * The centrepiece (WP 08 §2): three surfaces along the pipeline, a difference
 * surface, cuts, and a data-quality panel that makes the hard problem visible.
 */
export function VolTab({ ticker }: { ticker: string }) {
	const [stage, setStage] = useState<Stage>("raw");
	const raw = useIvSurface(ticker || null, "mid");
	const cleaned = useCleanedIvSurface(ticker || null);
	const local = useLocalVolSurface(ticker || null);

	const rawGrid = useMemo(
		() => (raw.data ? ivSurfaceToGrid(raw.data) : null),
		[raw.data],
	);
	const cleanedGrid = useMemo(
		() => (cleaned.data ? cleanedIvSurfaceToGrid(cleaned.data) : null),
		[cleaned.data],
	);
	const localGrid = useMemo(
		() => (local.data ? localVolSurfaceToGrid(local.data) : null),
		[local.data],
	);

	const active =
		stage === "raw" ? rawGrid : stage === "cleaned" ? cleanedGrid : localGrid;

	const diff = useMemo(
		() =>
			localGrid && cleanedGrid ? differenceGrid(localGrid, cleanedGrid) : null,
		[localGrid, cleanedGrid],
	);

	const violations = useMemo(
		() => (cleanedGrid ? checkArbitrage(cleanedGrid) : []),
		[cleanedGrid],
	);

	const smile = useMemo(() => {
		if (!active) return [];
		const midT = Math.floor(active.y.length / 2);
		return [
			{
				label: `T = ${active.y[midT]!.toFixed(2)}y`,
				x: Array.from(active.x),
				iv: Array.from(active.x, (_, xi) => {
					const v = zAt(active, xi, midT);
					return Number.isNaN(v) ? null : v;
				}),
			},
		];
	}, [active]);

	if (!ticker)
		return (
			<p className="text-sm text-ink-muted">Pick a ticker on the Prices tab.</p>
		);
	if (raw.error)
		return <ErrorState error={raw.error} onRetry={() => raw.refetch()} />;

	return (
		<div className="flex flex-col gap-4">
			<div className="flex items-center gap-2">
				<span className="text-2xs text-ink-muted uppercase">
					raw quotes → cleaning → smoothed IV → Dupire → local vol
				</span>
			</div>

			<div className="flex gap-1">
				{(["raw", "cleaned", "localvol"] as Stage[]).map((s) => (
					<button
						key={s}
						type="button"
						onClick={() => setStage(s)}
						className={
							"rounded-sm border px-2 py-1 text-xs " +
							(stage === s
								? "border-accent text-ink"
								: "border-hairline text-ink-secondary")
						}
					>
						{s === "raw" ? "Raw" : s === "cleaned" ? "Cleaned" : "Local vol"}
					</button>
				))}
			</div>

			<SurfaceView
				grid={active ?? undefined}
				title={`${ticker} — ${stage === "raw" ? "raw implied vol (holes are the information)" : stage === "cleaned" ? "cleaned & smoothed IV" : "Dupire local vol"}`}
			/>

			{diff && (
				<SurfaceView
					grid={diff}
					mode="divergent"
					title="Local vol − implied vol (divergent, grey at zero)"
					height={320}
				/>
			)}

			<SmileChart title="Smile (mid-maturity slice)" slices={smile} />

			{/* Data-quality panel — the part most projects avoid */}
			<div className="rounded-md border border-hairline bg-surface p-4">
				<h3 className="mb-2 text-sm font-medium text-ink">Data quality</h3>
				{cleaned.data && (
					<MetricRow className="border-0 bg-transparent p-0">
						<Metric
							label="Clean quotes"
							value={String(cleaned.data.n_clean_quotes)}
						/>
						<Metric
							label="Grid coverage"
							value={
								cleanedGrid
									? `${(coverage(cleanedGrid) * 100).toFixed(0)}%`
									: "—"
							}
						/>
						<Metric
							label="Raw surface holes"
							value={
								rawGrid ? `${(holeFraction(rawGrid) * 100).toFixed(0)}%` : "—"
							}
						/>
						<Metric
							label="Arbitrage flags"
							value={String(violations.length)}
							footnote="on total variance w = σ²T"
						/>
					</MetricRow>
				)}
				{cleaned.data && (
					<p className="mt-2 text-xs text-ink-secondary">
						{cleaned.data.cleaning_summary}
					</p>
				)}
				{violations.length > 0 && (
					<ul className="mt-2 flex flex-col gap-1">
						{violations.slice(0, 6).map((v, i) => (
							<li
								key={i}
								className="flex items-center gap-1.5 text-xs text-warning"
							>
								<AlertTriangle className="size-3.5" />
								<Badge tone="warning">{v.kind}</Badge>
								{v.detail}
							</li>
						))}
					</ul>
				)}
			</div>
		</div>
	);
}
