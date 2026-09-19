import type { PricingResponse } from "@/shared/api";
import { Uncertainty } from "@/shared/ui/density";

export function PriceResult({ result: r }: { result: PricingResponse }) {
	return (
		<div className="rounded-md border border-hairline bg-surface p-4">
			<span className="text-2xs text-ink-muted uppercase">Present value</span>
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
