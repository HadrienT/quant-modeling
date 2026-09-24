import { useEffect, useState } from "react";
import { useNavigate, useSearch } from "@tanstack/react-router";
import { useQuery, useQueryClient } from "@tanstack/react-query";
import { Plus } from "lucide-react";
import type { Portfolio } from "@/shared/api";
import { usePortfolioSnapshot } from "@/shared/api";
import { type Ledger, bookTrade, deleteTrade } from "@/shared/portfolio";
import { Badge, Button, toast } from "@/shared/ui";
import { EmptyState, ErrorState, TableSkeleton } from "@/shared/ui/states";
import { useMe } from "@/shared/session";
import { MigrationBanner } from "./MigrationBanner";
import { PnlSummary } from "./PnlSummary";
import { PortfolioMethodology } from "./PortfolioMethodology";
import { PortfolioTabs } from "./PortfolioTabs";
import { PortfolioToolbar } from "./PortfolioToolbar";
import { TradeForm, type TradePreset } from "./TradeForm";
import { useRepository } from "./useRepository";

/**
 * Portfolio (WP 09): a journal of trades, the positions it leaves, marked
 * to market every day. Stocks and derivatives alike; selling more than is
 * held opens a short. Valued server-side from the ledger, wherever the
 * portfolio is kept (this device or the account).
 */
export default function PortfolioPage() {
	const repo = useRepository();
	const me = useMe();
	const qc = useQueryClient();
	const search = useSearch({ from: "/portfolio" });
	const navigate = useNavigate({ from: "/portfolio" });
	const [trading, setTrading] = useState<TradePreset | "new" | null>(null);

	const list = useQuery({
		queryKey: ["portfolio", "list", repo.kind],
		queryFn: () => repo.list(),
	});
	const selectedId = search.id ?? list.data?.[0]?.id ?? null;
	const detailKey = ["portfolio", "detail", repo.kind, selectedId];
	const detail = useQuery({
		queryKey: detailKey,
		enabled: Boolean(selectedId),
		queryFn: () => repo.get(selectedId!),
	});
	const pf = detail.data;
	const snapshot = usePortfolioSnapshot(pf);
	const snap = snapshot.data;

	useEffect(() => {
		if (!search.id && list.data?.[0]) {
			navigate({ search: () => ({ id: list.data[0]!.id }) });
		}
	}, [search.id, list.data, navigate]);

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

	async function create() {
		const created = await repo.create("New portfolio");
		navigate({ search: () => ({ id: created.id }) });
		void list.refetch();
	}

	async function importJson(file: File) {
		try {
			const parsed = JSON.parse(await file.text()) as Portfolio;
			const created = await repo.importPortfolio(parsed);
			navigate({ search: () => ({ id: created.id }) });
			void list.refetch();
			toast.success(`Imported "${created.name}"`);
		} catch {
			toast.error("Not a valid portfolio JSON file");
		}
	}

	const hasTrades = (pf?.transactions?.length ?? 0) > 0;

	return (
		<div className="mx-auto flex max-w-[1400px] flex-col gap-4">
			<header className="flex flex-wrap items-center justify-between gap-3">
				<div className="flex items-center gap-2">
					<h1 className="text-lg font-semibold text-ink">Portfolio</h1>
					<Badge tone={repo.kind === "server" ? "accent" : "neutral"}>
						{repo.kind === "server" ? "server" : "this device only"}
					</Badge>
				</div>
				<PortfolioToolbar
					list={list.data ?? []}
					pf={pf}
					onSelect={(id) => navigate({ search: () => ({ id }) })}
					onCreate={() => void create()}
					onBaseCurrency={(c) =>
						pf &&
						void save({
							instruments: pf.instruments ?? [],
							transactions: pf.transactions ?? [],
							base_currency: c,
						})
					}
					onImport={(f) => void importJson(f)}
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
						<Button size="sm" onClick={() => void create()}>
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
						/>
					)}

					{snap && <PortfolioMethodology sections={snap.methodology} />}
				</>
			)}
		</div>
	);
}
