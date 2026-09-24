import { useState } from "react";
import { useQuery, useQueryClient } from "@tanstack/react-query";
import { Plus } from "lucide-react";
import { usePortfolioSnapshot } from "@/shared/api";
import {
	type Ledger,
	bookTrade,
	deletePosition,
	deleteTrade,
} from "@/shared/portfolio";
import { Button, toast } from "@/shared/ui";
import { EmptyState, ErrorState, TableSkeleton } from "@/shared/ui/states";
import { useMe } from "@/shared/session";
import { MigrationBanner } from "./MigrationBanner";
import { PnlSummary } from "./PnlSummary";
import { PortfolioMethodology } from "./PortfolioMethodology";
import { PortfolioSidebar } from "./PortfolioSidebar";
import { PortfolioTabs } from "./PortfolioTabs";
import { PortfolioToolbar } from "./PortfolioToolbar";
import { TradeForm, type TradePreset } from "./TradeForm";
import { usePortfolioBook } from "./usePortfolioBook";
import { useRepository } from "./useRepository";

/**
 * Portfolio (WP 09): a journal of trades, the positions it leaves, marked
 * to market every day. Stocks and derivatives alike; selling more than is
 * held opens a short. Valued server-side from the ledger, wherever the
 * portfolio is kept (this device or the account). The portfolios are
 * listed in a sidebar: select, rename, delete.
 */
export default function PortfolioPage() {
	const repo = useRepository();
	const me = useMe();
	const qc = useQueryClient();
	const book = usePortfolioBook(repo);
	const { list, selectedId } = book;
	const [trading, setTrading] = useState<TradePreset | "new" | null>(null);

	const detailKey = ["portfolio", "detail", repo.kind, selectedId];
	const detail = useQuery({
		queryKey: detailKey,
		enabled: Boolean(selectedId),
		queryFn: () => repo.get(selectedId!),
	});
	const pf = detail.data;
	const snapshot = usePortfolioSnapshot(pf);
	const snap = snapshot.data;

	async function save(ledger: Ledger) {
		if (!pf) return;
		try {
			const saved = await repo.putLedger(pf.id, ledger);
			qc.setQueryData(detailKey, saved);
			void list.refetch();
		} catch (e) {
			toast.error(e instanceof Error ? e.message : "Could not save the trade");
		}
	}

	const hasTrades = (pf?.transactions?.length ?? 0) > 0;

	return (
		<div className="mx-auto grid max-w-[1600px] gap-6 lg:grid-cols-[260px_minmax(0,1fr)]">
			{(list.data?.length ?? 0) > 0 && (
				<PortfolioSidebar
					portfolios={book.portfolios}
					selectedId={selectedId}
					storage={repo.kind}
					onSelect={book.select}
					onCreate={() => void book.create()}
					onRename={(id, name) => void book.rename(id, name)}
					onDelete={(id) => void book.remove(id)}
				/>
			)}
			<div className="flex min-w-0 flex-col gap-4">
				<header className="flex flex-wrap items-center justify-between gap-3">
					<h1 className="truncate text-lg font-semibold text-ink">
						{pf?.name ?? "Portfolio"}
					</h1>
					<PortfolioToolbar
						pf={pf}
						onBaseCurrency={(c) =>
							pf &&
							void save({
								instruments: pf.instruments ?? [],
								transactions: pf.transactions ?? [],
								base_currency: c,
							})
						}
						onImport={(f) => void book.importJson(f)}
					/>
				</header>

				{repo.kind === "local" && !me.data && <MigrationBanner />}

				{list.isLoading ? (
					<TableSkeleton />
				) : list.error ? (
					<ErrorState error={list.error} onRetry={() => list.refetch()} />
				) : !pf ? (
					<EmptyState
						kind="nothing-yet"
						title="No portfolio yet"
						description="Create one, then book trades: its positions and P&L follow."
						action={
							<Button size="sm" onClick={() => void book.create()}>
								Create portfolio
							</Button>
						}
					/>
				) : (
					<>
						{snapshot.error ? (
							<ErrorState
								error={snapshot.error}
								onRetry={() => snapshot.refetch()}
							/>
						) : snap ? (
							<PnlSummary snap={snap} />
						) : (
							hasTrades && <TableSkeleton />
						)}

						<div>
							<Button
								size="sm"
								onClick={() => setTrading(trading ? null : "new")}
							>
								<Plus className="size-3.5" /> New trade
							</Button>
						</div>
						{trading && (
							<TradeForm
								key={
									trading === "new"
										? "new"
										: `${trading.instrument.id}-${trading.side}`
								}
								preset={trading === "new" ? undefined : trading}
								onCancel={() => setTrading(null)}
								onBook={(instrument, draft) => {
									void save(bookTrade(pf, instrument, draft));
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
								onTrade={setTrading}
								onDelete={(id) => void save(deleteTrade(pf, id))}
								onDeletePosition={(id) => void save(deletePosition(pf, id))}
							/>
						)}

						{snap && <PortfolioMethodology sections={snap.methodology} />}
					</>
				)}
			</div>
		</div>
	);
}
