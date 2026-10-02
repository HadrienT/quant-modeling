import { useEffect, useRef } from "react";
import { deviceLabel, type PricingResult } from "@/shared/api";
import { ComputeTime, Uncertainty } from "@/shared/ui/density";
import { ScriptGreeks } from "./ScriptGreeks";

/** A scripted product's price, its Monte-Carlo error and how long it took,
 * then its greeks when they were asked for. Each new price is scrolled into
 * view: the form above it can be long. */
export function ScriptResult({ result: r }: { result: PricingResult }) {
	const ref = useRef<HTMLDivElement>(null);
	useEffect(() => {
		ref.current?.scrollIntoView?.({ behavior: "smooth", block: "nearest" });
	}, [r]);
	return (
		<div ref={ref} className="flex scroll-mt-20 flex-col gap-4">
			<div className="rounded-md border border-hairline bg-surface p-4">
				<div className="flex items-baseline justify-between gap-4">
					<span className="text-2xs text-ink-muted uppercase">
						Present value · on {deviceLabel(r)}
					</span>
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
			{r.risks && r.risks.length > 0 && <ScriptGreeks risks={r.risks} />}
		</div>
	);
}
