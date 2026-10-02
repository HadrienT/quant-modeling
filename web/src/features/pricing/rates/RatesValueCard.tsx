import { signedMoney } from "@/shared/format";
import { ComputeTime, EngineTag, Provenance } from "@/shared/ui";

/**
 * The value of a rates product with what it rests on: the engine, whether
 * the quotes are the market's or typed in, their date, and the compute time.
 * Cash-flow sign convention: a negative value is one the holder would pay to
 * get out of.
 */
export function RatesValueCard({
	npv,
	currency,
	engine,
	source,
	asOf,
	computeMs,
	roundTripMs,
	diagnostics,
}: {
	npv: number;
	currency: string;
	engine: string;
	source: "market" | "manual";
	asOf?: string | null;
	computeMs?: number | null;
	roundTripMs?: number;
	diagnostics?: string;
}) {
	return (
		<div className="rounded-md border border-hairline bg-surface p-4">
			<div className="flex items-baseline justify-between gap-4">
				<div className="flex flex-col gap-1">
					<span className="text-2xs text-ink-muted uppercase">
						Present value
					</span>
					<span className="flex items-baseline gap-1.5">
						<span className="font-mono text-xl text-ink">
							{signedMoney(npv)}
						</span>
						<span className="text-xs text-ink-secondary">{currency}</span>
					</span>
				</div>
				<div className="flex flex-col items-end gap-1">
					<EngineTag engine={engine} />
					<Provenance source={source} />
					{source === "market" && asOf && (
						<span className="text-2xs text-ink-muted">quotes of {asOf}</span>
					)}
					<ComputeTime computeMs={computeMs} roundTripMs={roundTripMs} />
				</div>
			</div>
			{diagnostics && (
				<p className="mt-2 font-mono text-2xs text-ink-muted">{diagnostics}</p>
			)}
		</div>
	);
}
