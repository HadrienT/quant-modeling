import { useMemo, useState } from "react";
import type { Portfolio, PortfolioSnapshot, PositionMark } from "@/shared/api";
import { aggregateRisk } from "@/shared/portfolio";
import { Tabs, TabsContent, TabsList, TabsTrigger } from "@/shared/ui";
import { DeletePositionDialog } from "./DeletePositionDialog";
import { HoldingsTable } from "./HoldingsTable";
import { PnlHistory } from "./PnlHistory";
import { RiskPanel } from "./RiskPanel";
import type { TradePreset } from "./TradeForm";
import { TransactionsTable } from "./TransactionsTable";

/** Positions, the journal, the P&L through time, and risk per underlying. */
export function PortfolioTabs({
	pf,
	snap,
	onTrade,
	onDelete,
	onDeletePosition,
}: {
	pf: Portfolio;
	snap: PortfolioSnapshot | undefined;
	onTrade: (preset: TradePreset) => void;
	onDelete: (tradeId: string) => void;
	onDeletePosition: (instrumentId: string) => void;
}) {
	const [deleting, setDeleting] = useState<PositionMark | null>(null);
	const positions = useMemo(() => snap?.positions ?? [], [snap]);
	const instruments = useMemo(() => pf.instruments ?? [], [pf]);
	const risk = useMemo(
		() => aggregateRisk(positions, instruments),
		[positions, instruments],
	);
	const currencies = useMemo(
		() =>
			Object.fromEntries(positions.map((p) => [p.instrument_id, p.currency])),
		[positions],
	);

	const tradesOf = (id: string | undefined) =>
		(pf.transactions ?? []).filter((t) => t.instrument_id === id).length;

	return (
		<Tabs defaultValue="positions">
			<DeletePositionDialog
				position={deleting}
				trades={tradesOf(deleting?.instrument_id)}
				onCancel={() => setDeleting(null)}
				onConfirm={() => {
					if (deleting) onDeletePosition(deleting.instrument_id);
					setDeleting(null);
				}}
			/>
			<TabsList>
				<TabsTrigger value="positions">Positions</TabsTrigger>
				<TabsTrigger value="transactions">
					Transactions ({pf.transactions?.length ?? 0})
				</TabsTrigger>
				<TabsTrigger value="history">P&L history</TabsTrigger>
				<TabsTrigger value="risk">Risk</TabsTrigger>
			</TabsList>
			<TabsContent value="positions">
				<HoldingsTable
					positions={positions}
					base={pf.base_currency}
					onDelete={setDeleting}
					onTrade={(p, side) => {
						const instrument = instruments.find(
							(i) => i.id === p.instrument_id,
						);
						if (!instrument) return;
						// Prefilled to close the position; any quantity can be typed.
						const closing =
							(side === "sell" && p.quantity > 0) ||
							(side === "buy" && p.quantity < 0);
						onTrade({
							instrument,
							side,
							quantity: closing ? Math.abs(p.quantity) : 1,
						});
					}}
				/>
			</TabsContent>
			<TabsContent value="transactions">
				<TransactionsTable
					trades={pf.transactions ?? []}
					instruments={instruments}
					currencies={currencies}
					onDelete={onDelete}
				/>
			</TabsContent>
			<TabsContent value="history">
				<PnlHistory pf={pf} />
			</TabsContent>
			<TabsContent value="risk">
				<RiskPanel risk={risk} base={pf.base_currency} />
			</TabsContent>
		</Tabs>
	);
}
