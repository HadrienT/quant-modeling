import type { PortfolioSnapshot } from "@/shared/api";
import { DeltaBadge, Metric, MetricRow, NumberCell } from "@/shared/ui";

/**
 * The portfolio's numbers today, in its base currency. Total P&L is
 * realised + unrealised since the first trade, fees included — the same
 * number the P&L history ends on.
 */
export function PnlSummary({ snap }: { snap: PortfolioSnapshot }) {
	const ccy = snap.base_currency;
	return (
		<div className="flex flex-col gap-2">
			<MetricRow className="xl:grid-cols-6">
				<Metric
					label="Market value"
					unit={ccy}
					value={<NumberCell value={snap.market_value} className="text-xl" />}
					footnote={`marked ${snap.as_of}`}
				/>
				<Metric
					label="Day P&L"
					unit={ccy}
					value={<DeltaBadge value={snap.day_pnl} className="text-xl" />}
					footnote="since the previous business day's close"
				/>
				<Metric
					label="Unrealised"
					unit={ccy}
					value={<DeltaBadge value={snap.unrealised} className="text-xl" />}
					footnote="open positions vs average cost"
				/>
				<Metric
					label="Realised"
					unit={ccy}
					value={<DeltaBadge value={snap.realised} className="text-xl" />}
					footnote="closed quantities, net of fees"
				/>
				<Metric
					label="Total P&L"
					unit={ccy}
					value={<DeltaBadge value={snap.total_pnl} className="text-xl" />}
				/>
				<Metric
					label="Fees paid"
					unit={ccy}
					value={<NumberCell value={snap.fees} className="text-xl" />}
				/>
			</MetricRow>
			{snap.warnings.length > 0 && (
				<ul className="flex flex-col gap-1 rounded-sm border border-hairline bg-surface-raised px-3 py-2 text-xs text-warning">
					{snap.warnings.map((w) => (
						<li key={w}>{w}</li>
					))}
				</ul>
			)}
		</div>
	);
}
