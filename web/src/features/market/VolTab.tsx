import { useDeltaSurface } from "@/shared/api";
import { ErrorState } from "@/shared/ui/states";
import {
	AtmTermStructureChart,
	RiskReversalButterflyChart,
} from "@/shared/viz";
import { DeltaMatrix } from "./DeltaMatrix";
import { SurfaceDiagnostics } from "./SurfaceDiagnostics";

/**
 * Volatility tab. Primary view is the desk-style delta-bucketed vol matrix
 * (10/25-delta put, ATM, 25/10-delta call) plus the ATM term structure and
 * risk-reversal/butterfly charts -- what an options desk actually reads:
 * dense numbers in delta space, and skew/convexity as the two numbers a
 * desk quotes them as (RR, BF), not a strike-indexed surface plot. A real
 * desk screen (Bloomberg OVML and similar) is a delta/tenor matrix first,
 * a 3D surface rarely if ever, and never the unfitted raw quote scatter.
 *
 * The full raw -> cleaned -> local-vol pipeline (including the 3D surface
 * and the data-quality/arbitrage panel) is still here -- it just moved to
 * SurfaceDiagnostics, a secondary section: valuable for a quant checking
 * data quality, not what a trader opens first.
 */
export function VolTab({
	ticker,
	unavailable,
}: {
	ticker: string;
	/** Why this market has no volatility surface (no option data), if so. */
	unavailable?: string | null;
}) {
	// Not asked for at all when the market has no option chains: a request
	// that can only fail would show an error for what is a known absence.
	const delta = useDeltaSurface(unavailable ? null : ticker || null);

	if (unavailable)
		return (
			<p className="rounded-sm border border-hairline bg-surface p-3 text-sm text-ink-secondary">
				{unavailable}
			</p>
		);

	if (!ticker)
		return (
			<p className="text-sm text-ink-muted">Pick a ticker on the Prices tab.</p>
		);
	if (delta.error)
		return <ErrorState error={delta.error} onRetry={() => delta.refetch()} />;

	const rows = delta.data?.rows;
	// ATM is reachable for essentially every slice (k=0 needs no delta
	// solve), so this filter rarely drops anything; RR/BF do it per-series
	// instead (RiskReversalButterflyChart), since a maturity can have a
	// reachable 25-delta bucket but not a 10-delta one.
	const termStructure = rows
		?.filter((r) => r.vol_atm != null)
		.map((r) => ({ ttm: r.ttm, vol: r.vol_atm! }));
	const skew = rows?.map((r) => ({
		ttm: r.ttm,
		rr25: r.rr25 ?? null,
		bf25: r.bf25 ?? null,
		rr10: r.rr10 ?? null,
		bf10: r.bf10 ?? null,
	}));

	return (
		<div className="flex flex-col gap-6">
			<section className="flex flex-col gap-3">
				<h2 className="text-sm font-medium text-ink">
					{ticker} — delta-bucketed vol matrix
				</h2>
				<DeltaMatrix rows={rows} />
			</section>

			<div className="grid grid-cols-1 gap-4 lg:grid-cols-2">
				<AtmTermStructureChart
					points={termStructure}
					isLoading={delta.isLoading}
					error={delta.error}
				/>
				<RiskReversalButterflyChart
					points={skew}
					isLoading={delta.isLoading}
					error={delta.error}
				/>
			</div>

			<details className="group">
				<summary className="cursor-pointer text-sm font-medium text-ink-secondary select-none hover:text-ink">
					Surface diagnostics (raw → cleaned → local vol, data quality)
				</summary>
				<div className="mt-3">
					<SurfaceDiagnostics ticker={ticker} />
				</div>
			</details>
		</div>
	);
}
