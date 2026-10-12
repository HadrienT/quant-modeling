import { Pencil, Trash2 } from "lucide-react";
import type { Counterparty, Portfolio } from "@/shared/api";
import { SECTORS, describeCsa, tradedUnder } from "@/shared/portfolio";
import { Badge, Button } from "@/shared/ui";

/**
 * One counterparty: what its credit is read from, and for each netting set
 * the collateral terms and the trades that close out together under it.
 */
export function CounterpartyCard({
	pf,
	counterparty: c,
	onEdit,
	onDelete,
}: {
	pf: Portfolio;
	counterparty: Counterparty;
	/** absent for a read-only portfolio */
	onEdit?: () => void;
	onDelete?: () => void;
}) {
	const sets = c.netting_sets ?? [];
	const inUse = sets.some((n) => tradedUnder(pf, n.id).length > 0);
	const sector = SECTORS.find((s) => s.value === c.sector)?.label ?? c.sector;

	return (
		<article
			aria-label={c.name}
			className="flex flex-col gap-3 rounded-md border border-hairline bg-surface p-4"
		>
			<header className="flex flex-wrap items-center justify-between gap-2">
				<div className="flex flex-wrap items-center gap-2">
					<h3 className="text-sm font-semibold text-ink">{c.name}</h3>
					<Badge>{c.rating}</Badge>
					<span className="text-xs text-ink-secondary">
						{sector} · recovery {Math.round((c.recovery ?? 0.4) * 100)}%
					</span>
				</div>
				{onEdit && onDelete && (
					<div className="flex gap-1">
						<Button size="sm" variant="ghost" onClick={onEdit}>
							<Pencil className="size-3.5" /> Edit
						</Button>
						<Button
							size="sm"
							variant="ghost"
							disabled={inUse}
							title={
								inUse
									? "Trades are under this counterparty's netting sets: move them first"
									: "Delete this counterparty"
							}
							onClick={onDelete}
						>
							<Trash2 className="size-3.5" /> Delete
						</Button>
					</div>
				)}
			</header>
			<ul className="flex flex-col gap-2">
				{sets.map((n) => {
					const trades = tradedUnder(pf, n.id);
					return (
						<li
							key={n.id}
							className="flex flex-col gap-1 border-t border-hairline pt-2"
						>
							<span className="text-sm text-ink">{n.name}</span>
							<span className="text-xs text-ink-secondary">
								{describeCsa(n.csa)}
							</span>
							<span className="text-xs text-ink-muted">
								{trades.length === 0
									? "No trade under this netting set"
									: `${trades.length} trade${trades.length > 1 ? "s" : ""}: ${trades.map((t) => t.label || t.id).join("; ")}`}
							</span>
						</li>
					);
				})}
			</ul>
		</article>
	);
}
