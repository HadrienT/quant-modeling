import type { PricingResult } from "@/shared/api";
import { ComputeTime, Uncertainty } from "@/shared/ui/density";

export function PriceResult({ result: r }: { result: PricingResult }) {
	return (
		<div className="rounded-md border border-hairline bg-surface p-4">
			<div className="flex items-baseline justify-between gap-4">
				<span className="text-2xs text-ink-muted uppercase">Present value</span>
				<ComputeTime computeMs={r.compute_ms} roundTripMs={r.round_trip_ms} />
			</div>
			<div className="mt-1">
				<Uncertainty
					value={r.npv}
					stdError={r.mc_std_error}
					magnitude="price"
					emphasis
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
