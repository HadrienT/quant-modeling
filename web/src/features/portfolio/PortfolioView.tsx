import { useState } from "react";
import { Plus } from "lucide-react";
import type { Portfolio } from "@/shared/api";
import { usePortfolioSnapshot } from "@/shared/api";
import {
	type Ledger,
	bookTrade,
	deletePosition,
	deleteTrade,
} from "@/shared/portfolio";
import { Button } from "@/shared/ui";
import { ErrorState, TableSkeleton } from "@/shared/ui/states";
import { PnlSummary } from "./PnlSummary";
import { PortfolioMethodology } from "./PortfolioMethodology";
import { PortfolioTabs } from "./PortfolioTabs";
import { TradeForm, type TradePreset } from "./TradeForm";

/**
 * One portfolio, valued: today's numbers, the trade form, positions,
 * journal, P&L history, risk and methodology. Read-only (a demo): the same
 * view without anything that edits the ledger.
 */
export function PortfolioView({
	pf,
	onSave,
}: {
	pf: Portfolio;
	/** absent for a read-only portfolio */
	onSave?: (ledger: Ledger) => void;
}) {
	const [trading, setTrading] = useState<TradePreset | "new" | null>(null);
	const snapshot = usePortfolioSnapshot(pf);
	const snap = snapshot.data;
	const hasTrades = (pf.transactions?.length ?? 0) > 0;

	return (
		<>
			{snapshot.error ? (
				<ErrorState error={snapshot.error} onRetry={() => snapshot.refetch()} />
			) : snap ? (
				<PnlSummary snap={snap} />
			) : (
				hasTrades && <TableSkeleton />
			)}

			{onSave && (
				<div>
					<Button size="sm" onClick={() => setTrading(trading ? null : "new")}>
						<Plus className="size-3.5" /> New trade
					</Button>
				</div>
			)}
			{onSave && trading && (
				<TradeForm
					key={
						trading === "new"
							? "new"
							: `${trading.instrument.id}-${trading.side}`
					}
					preset={trading === "new" ? undefined : trading}
					onCancel={() => setTrading(null)}
					onBook={(instrument, draft) => {
						onSave(bookTrade(pf, instrument, draft));
						setTrading(null);
					}}
				/>
			)}

			{!hasTrades ? (
				<p className="text-sm text-ink-muted">
					No trades yet. Book a purchase (or a sale, to open a short).
				</p>
			) : (
				<PortfolioTabs
					pf={pf}
					snap={snap}
					readOnly={!onSave}
					onTrade={setTrading}
					onDelete={(id) => onSave?.(deleteTrade(pf, id))}
					onDeletePosition={(id) => onSave?.(deletePosition(pf, id))}
				/>
			)}

			{snap && <PortfolioMethodology sections={snap.methodology} />}
		</>
	);
}
