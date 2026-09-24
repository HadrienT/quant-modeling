import { useQuery, useQueryClient } from "@tanstack/react-query";
import { usePortfolioDemos } from "@/shared/api";
import type { Ledger } from "@/shared/portfolio";
import { Button, toast } from "@/shared/ui";
import { EmptyState, ErrorState, TableSkeleton } from "@/shared/ui/states";
import { useMe } from "@/shared/session";
import { DemoBanner } from "./DemoBanner";
import { MigrationBanner } from "./MigrationBanner";
import { PortfolioSidebar } from "./PortfolioSidebar";
import { PortfolioToolbar } from "./PortfolioToolbar";
import { PortfolioView } from "./PortfolioView";
import { usePortfolioBook } from "./usePortfolioBook";
import { useRepository } from "./useRepository";

/**
 * Portfolio (WP 09): a journal of trades, the positions it leaves, marked
 * to market every day. Stocks and derivatives alike; selling more than is
 * held opens a short. Valued server-side from the ledger, wherever the
 * portfolio is kept (this device or the account). The sidebar lists one's
 * portfolios (select, rename, delete) and the read-only demos.
 */
export default function PortfolioPage() {
	const repo = useRepository();
	const me = useMe();
	const qc = useQueryClient();
	const book = usePortfolioBook(repo);
	const { list, selectedId, demoId } = book;
	const demos = usePortfolioDemos();
	const demo = demoId
		? demos.data?.find((d) => d.portfolio.id === demoId)
		: undefined;

	const detailKey = ["portfolio", "detail", repo.kind, selectedId];
	const detail = useQuery({
		queryKey: detailKey,
		enabled: Boolean(selectedId) && !demoId,
		queryFn: () => repo.get(selectedId!),
	});
	const pf = demoId ? demo?.portfolio : detail.data;

	async function save(ledger: Ledger) {
		if (!detail.data) return;
		try {
			const saved = await repo.putLedger(detail.data.id, ledger);
			qc.setQueryData(detailKey, saved);
			void list.refetch();
		} catch (e) {
			toast.error(e instanceof Error ? e.message : "Could not save the trade");
		}
	}

	return (
		<div className="mx-auto grid max-w-[1600px] gap-6 lg:grid-cols-[260px_minmax(0,1fr)]">
			{list.data && (
				<PortfolioSidebar
					portfolios={book.portfolios}
					selectedId={selectedId}
					demos={demos.data ?? []}
					demosLoading={demos.isLoading}
					demoId={demoId}
					onSelectDemo={book.selectDemo}
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
					{!demoId && (
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
					)}
				</header>

				{demo && (
					<DemoBanner
						description={demo.description}
						onCopy={() => void book.copyDemo(demo.portfolio)}
					/>
				)}
				{!demoId && repo.kind === "local" && !me.data && <MigrationBanner />}

				{list.isLoading || (demoId && demos.isLoading) ? (
					<TableSkeleton />
				) : list.error ? (
					<ErrorState error={list.error} onRetry={() => list.refetch()} />
				) : demoId && demos.error ? (
					<ErrorState error={demos.error} onRetry={() => demos.refetch()} />
				) : !pf ? (
					<EmptyState
						kind="nothing-yet"
						title={demoId ? "Demo not found" : "No portfolio yet"}
						description={
							demoId
								? "This demo is not available right now."
								: "Create one and book trades — or open a demo to see a valued portfolio first."
						}
						action={
							<Button size="sm" onClick={() => void book.create()}>
								Create portfolio
							</Button>
						}
					/>
				) : (
					<PortfolioView
						key={pf.id}
						pf={pf}
						onSave={demoId ? undefined : (l) => void save(l)}
					/>
				)}
			</div>
		</div>
	);
}
