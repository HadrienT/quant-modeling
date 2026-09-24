import { useState } from "react";
import { FolderOpen, Plus } from "lucide-react";
import type { PortfolioSummary } from "@/shared/api";
import {
	Badge,
	Button,
	Dialog,
	DialogContent,
	DialogDescription,
	DialogHeader,
	DialogTitle,
} from "@/shared/ui";
import { PortfolioListItem } from "./PortfolioListItem";

/**
 * Every portfolio, one click away: name, open positions, last edit. Rename
 * in place; delete after a confirmation that says what is lost.
 */
export function PortfolioSidebar({
	portfolios,
	selectedId,
	storage,
	onSelect,
	onCreate,
	onRename,
	onDelete,
}: {
	portfolios: PortfolioSummary[];
	selectedId: string | null;
	storage: "local" | "server";
	onSelect: (id: string) => void;
	onCreate: () => void;
	onRename: (id: string, name: string) => void;
	onDelete: (id: string) => void;
}) {
	const [deleting, setDeleting] = useState<PortfolioSummary | null>(null);

	return (
		<aside
			aria-label="Portfolios"
			className="flex flex-col gap-3 lg:sticky lg:top-4 lg:self-start"
		>
			<div className="flex items-center justify-between gap-2">
				<h2 className="flex items-center gap-1.5 text-xs font-semibold tracking-wide text-ink-secondary uppercase">
					<FolderOpen className="size-3.5" /> Portfolios
				</h2>
				<Badge tone={storage === "server" ? "accent" : "neutral"}>
					{storage === "server" ? "account" : "this device"}
				</Badge>
			</div>
			<ul className="flex flex-col gap-1">
				{portfolios.map((p) => (
					<PortfolioListItem
						key={p.id}
						p={p}
						active={p.id === selectedId}
						onSelect={() => onSelect(p.id)}
						onRename={(name) => onRename(p.id, name)}
						onDelete={() => setDeleting(p)}
					/>
				))}
			</ul>
			<Button size="sm" variant="secondary" onClick={onCreate}>
				<Plus className="size-3.5" /> New portfolio
			</Button>

			<Dialog
				open={!!deleting}
				onOpenChange={(open) => !open && setDeleting(null)}
			>
				<DialogContent>
					<DialogHeader>
						<DialogTitle>Delete “{deleting?.name}”?</DialogTitle>
						<DialogDescription>
							Its journal of trades and its P&L history are deleted with it.
							This cannot be undone — export it as JSON first to keep a copy.
						</DialogDescription>
					</DialogHeader>
					<div className="mt-4 flex justify-end gap-2">
						<Button size="sm" variant="ghost" onClick={() => setDeleting(null)}>
							Keep it
						</Button>
						<Button
							size="sm"
							variant="danger"
							onClick={() => {
								if (deleting) onDelete(deleting.id);
								setDeleting(null);
							}}
						>
							Delete portfolio
						</Button>
					</div>
				</DialogContent>
			</Dialog>
		</aside>
	);
}
