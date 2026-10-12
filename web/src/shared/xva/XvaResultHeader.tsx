import type { XvaResponse } from "@/shared/api";
import { Badge } from "@/shared/ui";

/**
 * What a run of the exposure engine was: the netting set, the curve it was
 * simulated on, how many paths, where and how long — and what to read in it.
 */
export function XvaResultHeader({ data }: { data: XvaResponse }) {
	return (
		<div className="flex flex-col gap-2 rounded-sm border border-hairline bg-surface p-3">
			<div className="flex flex-wrap items-center gap-2">
				<h2 className="text-sm font-semibold text-ink">
					{data.portfolio_label}
				</h2>
				<Badge>{data.market.curve_label}</Badge>
				<span className="text-xs text-ink-muted">
					as of {data.market.curve_as_of} · {data.paths.toLocaleString("en-US")}{" "}
					paths · {(data.compute_ms / 1000).toFixed(1)} s on the{" "}
					{data.device === "gpu" ? "GPU" : "CPU"}
				</span>
			</div>
			<p className="text-sm leading-relaxed text-ink-secondary">
				{data.lesson}
			</p>
			<p className="text-xs text-ink-muted">{data.device_reason}</p>
		</div>
	);
}
