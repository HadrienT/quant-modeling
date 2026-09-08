import { ChartFrame } from "../ChartFrame";
import { useChartTheme } from "../theme";

export type AllocationRow = {
	label: string;
	weight: number;
	sublabel?: string;
	returnPct?: number | null;
};

/** Sorted horizontal bars. Never a pie (blueprint WP 06 §2). */
export function AllocationChart({
	rows,
	title = "Allocation",
	isLoading,
	error,
}: {
	rows?: AllocationRow[];
	title?: string;
	isLoading?: boolean;
	error?: unknown;
}) {
	const t = useChartTheme();
	const sorted = [...(rows ?? [])].sort((a, b) => b.weight - a.weight);
	const max = Math.max(...sorted.map((r) => r.weight), 0.0001);

	return (
		<ChartFrame
			title={title}
			isLoading={isLoading}
			error={error}
			isEmpty={sorted.length === 0}
			height={Math.max(120, sorted.length * 34)}
			table={{
				columns: ["Name", "Weight", "Return"],
				rows: sorted.map((r) => [
					r.label,
					`${(r.weight * 100).toFixed(1)}%`,
					r.returnPct == null ? "n/a" : `${r.returnPct.toFixed(1)}%`,
				]),
			}}
		>
			<ul className="flex flex-col gap-2">
				{sorted.map((r) => (
					<li key={r.label} className="flex items-center gap-3 text-xs">
						<span className="w-24 shrink-0 truncate text-ink" title={r.label}>
							{r.label}
						</span>
						<span className="relative h-4 flex-1 overflow-hidden rounded-xs bg-surface-raised">
							<span
								className="absolute inset-y-0 left-0"
								style={{
									width: `${(r.weight / max) * 100}%`,
									background: t.series[0],
								}}
							/>
						</span>
						<span className="w-12 shrink-0 text-right font-mono text-ink tabular-nums">
							{(r.weight * 100).toFixed(1)}%
						</span>
						{r.returnPct != null && (
							<span
								className="w-14 shrink-0 text-right font-mono tabular-nums"
								style={{ color: r.returnPct >= 0 ? t.pnl.up : t.pnl.down }}
							>
								{r.returnPct >= 0 ? "+" : "−"}
								{Math.abs(r.returnPct).toFixed(1)}%
							</span>
						)}
					</li>
				))}
			</ul>
		</ChartFrame>
	);
}
