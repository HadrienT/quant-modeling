import { useCallback, useEffect, useMemo, useState } from "react";
import { useNavigate, useSearch } from "@tanstack/react-router";
import { useQuery } from "@tanstack/react-query";
import { Download, Plus, Upload } from "lucide-react";
import type { Portfolio, Position } from "@/shared/api";
import { usePricePortfolio } from "@/shared/api";
import { aggregateRisk } from "@/shared/portfolio";
import { Badge, Button, Input, toast } from "@/shared/ui";
import { EmptyState, ErrorState, TableSkeleton } from "@/shared/ui/states";
import { useMe } from "@/shared/session";
import { AddPosition } from "./AddPosition";
import { PositionsTable } from "./PositionsTable";
import { RiskPanel } from "./RiskPanel";
import { MigrationBanner } from "./MigrationBanner";
import { useRepository } from "./useRepository";

/** Portfolio & risk (WP 09). No product-catalog file of its own — reuses shared/products. */
export default function PortfolioPage() {
	const repo = useRepository();
	const me = useMe();
	const search = useSearch({ from: "/portfolio" });
	const navigate = useNavigate({ from: "/portfolio" });
	const [adding, setAdding] = useState(false);

	const list = useQuery({
		queryKey: ["portfolio", "list", repo.kind],
		queryFn: () => repo.list(),
	});

	const selectedId = search.id ?? list.data?.[0]?.id ?? null;

	const detail = useQuery({
		queryKey: ["portfolio", "detail", repo.kind, selectedId],
		enabled: Boolean(selectedId),
		queryFn: () => repo.get(selectedId!),
	});

	const price = usePricePortfolio();

	const pf = detail.data;
	const positions = useMemo(() => pf?.positions ?? [], [pf]);
	const risk = useMemo(() => aggregateRisk(positions), [positions]);

	const refresh = useCallback(() => {
		void list.refetch();
		void detail.refetch();
	}, [list, detail]);

	useEffect(() => {
		if (!search.id && list.data?.[0]) {
			navigate({ search: () => ({ id: list.data[0]!.id }) });
		}
	}, [search.id, list.data, navigate]);

	async function savePositions(next: Position[]) {
		if (!pf) return;
		await repo.putPositions(pf.id, next);
		refresh();
	}

	function exportJson() {
		if (!pf) return;
		const blob = new Blob([JSON.stringify(pf, null, 2)], {
			type: "application/json",
		});
		const a = document.createElement("a");
		a.href = URL.createObjectURL(blob);
		a.download = `${pf.name}.json`;
		a.click();
	}

	function exportCsv() {
		if (!pf) return;
		const rows = [
			["instrument", "direction", "quantity", "entry", "npv", "engine"],
			...positions.map((p) => [
				p.label,
				p.direction,
				p.quantity,
				p.entry_price,
				p.result?.npv ?? "",
				p.result?.engine ?? "",
			]),
		];
		const csv = rows.map((r) => r.join(",")).join("\n");
		const a = document.createElement("a");
		a.href = URL.createObjectURL(new Blob([csv], { type: "text/csv" }));
		a.download = `${pf.name}.csv`;
		a.click();
	}

	async function importJson(file: File) {
		try {
			const parsed = JSON.parse(await file.text()) as Portfolio;
			const created = await repo.importPortfolio(parsed);
			navigate({ search: () => ({ id: created.id }) });
			refresh();
			toast.success(`Imported "${created.name}"`);
		} catch {
			toast.error("Not a valid portfolio JSON file");
		}
	}

	return (
		<div className="mx-auto flex max-w-[1400px] flex-col gap-4">
			<header className="flex flex-wrap items-center justify-between gap-3">
				<div className="flex items-center gap-2">
					<h1 className="text-lg font-semibold text-ink">Portfolio</h1>
					<Badge tone={repo.kind === "server" ? "accent" : "neutral"}>
						{repo.kind === "server" ? "server" : "this device only"}
					</Badge>
				</div>
				<div className="flex items-center gap-1.5">
					<select
						className="h-8 rounded-sm border border-hairline bg-surface px-2 text-xs text-ink"
						value={selectedId ?? ""}
						onChange={(e) =>
							navigate({ search: () => ({ id: e.target.value }) })
						}
					>
						{(list.data ?? []).map((p) => (
							<option key={p.id} value={p.id}>
								{p.name} ({p.n_positions})
							</option>
						))}
					</select>
					<Button
						size="sm"
						variant="ghost"
						onClick={async () => {
							const created = await repo.create("New portfolio");
							navigate({ search: () => ({ id: created.id }) });
							refresh();
						}}
					>
						<Plus className="size-3.5" /> New
					</Button>
					<Button size="sm" variant="ghost" onClick={exportJson} disabled={!pf}>
						<Download className="size-3.5" /> JSON
					</Button>
					<Button size="sm" variant="ghost" onClick={exportCsv} disabled={!pf}>
						<Download className="size-3.5" /> CSV
					</Button>
					<label className="inline-flex cursor-pointer items-center gap-1 rounded-sm px-2 py-1 text-xs text-ink-secondary hover:text-ink">
						<Upload className="size-3.5" /> Import
						<Input
							type="file"
							accept="application/json"
							className="hidden"
							onChange={(e) =>
								e.target.files?.[0] && importJson(e.target.files[0])
							}
						/>
					</label>
				</div>
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
					description="Create one to start tracking positions and risk."
					action={
						<Button
							size="sm"
							onClick={async () => {
								const created = await repo.create("New portfolio");
								navigate({ search: () => ({ id: created.id }) });
								refresh();
							}}
						>
							Create portfolio
						</Button>
					}
				/>
			) : (
				<>
					<div className="flex items-center gap-2">
						<Button
							size="sm"
							onClick={() =>
								price.mutate(pf.id, {
									onSuccess: () => refresh(),
								})
							}
							disabled={price.isPending || repo.kind === "local"}
						>
							{price.isPending ? "Pricing…" : "Price all positions"}
						</Button>
						{repo.kind === "local" && (
							<span className="text-2xs text-ink-muted">
								Batch pricing needs a server portfolio.
							</span>
						)}
						<Button
							size="sm"
							variant="secondary"
							onClick={() => setAdding((v) => !v)}
						>
							<Plus className="size-3.5" /> Add position
						</Button>
					</div>

					{adding && (
						<AddPosition
							onAdd={(pos) => {
								void savePositions([...positions, pos]);
								setAdding(false);
							}}
						/>
					)}

					<RiskPanel risk={risk} />

					<PositionsTable positions={positions} onChange={savePositions} />
				</>
			)}
		</div>
	);
}
