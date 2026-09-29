import { useCreditSpreads } from "@/shared/api";
import { Methodology, Segmented, WarningList } from "@/shared/ui";
import { ChartSkeleton, ErrorState } from "@/shared/ui/states";
import { pct } from "./format";
import { RatingsTable } from "./RatingsTable";
import { SpreadHistoryPanel } from "./SpreadHistoryPanel";
import { TermStructurePanel } from "./TermStructurePanel";

const RECOVERIES = [0.25, 0.4, 0.6];

/**
 * Market credit curves: the investment-grade spread term structure and the
 * hazard curve bootstrapped from it, spreads and default probabilities by
 * rating, the history of each series, and the methodology — all computed by
 * api/app/credit.py with the C++ CDS bootstrap.
 */
export function SpreadsTab({
	recovery,
	onRecovery,
}: {
	recovery: number;
	onRecovery: (r: number) => void;
}) {
	const spreads = useCreditSpreads(recovery);
	const data = spreads.data;

	return (
		<div className="flex flex-col gap-6">
			<div className="flex flex-wrap items-center gap-2 text-xs text-ink-secondary">
				<span>Recovery assumption</span>
				<Segmented
					label="Recovery rate"
					options={RECOVERIES.map((r) => ({
						value: r,
						label: pct(r, 0),
						title:
							r === 0.4
								? "Market convention for senior unsecured debt"
								: undefined,
					}))}
					value={recovery}
					onChange={onRecovery}
				/>
			</div>
			{spreads.error ? (
				<ErrorState error={spreads.error} onRetry={() => spreads.refetch()} />
			) : !data ? (
				<ChartSkeleton />
			) : (
				<>
					<WarningList warnings={data.warnings} />
					<TermStructurePanel data={data} />
					<RatingsTable data={data} />
					<SpreadHistoryPanel data={data} />
					<Methodology
						id="credit-curves-methodology"
						sections={data.methodology}
					/>
				</>
			)}
		</div>
	);
}
