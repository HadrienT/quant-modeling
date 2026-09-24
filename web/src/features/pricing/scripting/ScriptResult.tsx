import type { PricingResult } from "@/shared/api";
import { ComputeTime, Uncertainty } from "@/shared/ui/density";

/** A scripted product's price, its Monte-Carlo error and how long it took. */
export function ScriptResult({ result: r }: { result: PricingResult }) {
	return (
		<div className="rounded-md border border-hairline bg-surface p-4">
			<div className="flex items-baseline justify-between gap-4">
				<span className="text-2xs text-ink-muted uppercase">Present value</span>
				<ComputeTime computeMs={r.compute_ms} roundTripMs={r.round_trip_ms} />
			</div>
			<div className="text-xl">
				<Uncertainty
					value={r.npv}
					stdError={r.mc_std_error}
					magnitude="price"
				/>
			</div>
			{r.diagnostics && (
				<p className="mt-2 font-mono text-2xs text-ink-muted">
					{r.diagnostics}
				</p>
			)}
		</div>
	);
}
