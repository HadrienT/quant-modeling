import { useEffect, useRef, useState } from "react";
import { Check, Pencil, Trash2, X } from "lucide-react";
import type { PortfolioSummary } from "@/shared/api";
import { formatRelative } from "@/shared/format";
import { Input, cn } from "@/shared/ui";

/** Server timestamps are naive UTC; the browser would read them as local. */
const asUtc = (s: string) => (/Z|[+-]\d\d:\d\d$/.test(s) ? s : `${s}Z`);

/** One portfolio in the sidebar: select it, rename it in place, delete it. */
export function PortfolioListItem({
	p,
	active,
	onSelect,
	onRename,
	onDelete,
}: {
	p: PortfolioSummary;
	active: boolean;
	onSelect: () => void;
	onRename: (name: string) => void;
	onDelete: () => void;
}) {
	const [editing, setEditing] = useState(false);
	const [name, setName] = useState(p.name);
	const input = useRef<HTMLInputElement>(null);
	// Focus the field the user just asked to edit (not on page load).
	useEffect(() => {
		if (editing) input.current?.select();
	}, [editing]);

	if (editing) {
		const commit = () => {
			const next = name.trim();
			if (next && next !== p.name) onRename(next);
			setEditing(false);
		};
		return (
			<li className="flex items-center gap-1 rounded-md border border-accent bg-surface-raised p-1.5">
				<Input
					aria-label="Portfolio name"
					value={name}
					maxLength={80}
					ref={input}
					className="h-8"
					onChange={(e) => setName(e.target.value)}
					onKeyDown={(e) => {
						if (e.key === "Enter") commit();
						if (e.key === "Escape") setEditing(false);
					}}
				/>
				<button
					type="button"
					aria-label="Save name"
					className="p-1 text-good"
					onClick={commit}
				>
					<Check className="size-4" />
				</button>
				<button
					type="button"
					aria-label="Cancel rename"
					className="p-1 text-ink-muted hover:text-ink"
					onClick={() => setEditing(false)}
				>
					<X className="size-4" />
				</button>
			</li>
		);
	}

	return (
		<li
			className={cn(
				"group relative flex items-center rounded-md border transition-colors",
				active
					? "border-accent bg-[color-mix(in_srgb,var(--color-accent)_10%,transparent)]"
					: "border-transparent hover:border-hairline hover:bg-surface-raised",
			)}
		>
			{active && (
				<span
					aria-hidden="true"
					className="absolute inset-y-2 left-0 w-0.5 rounded-full bg-accent"
				/>
			)}
			<button
				type="button"
				aria-current={active ? "page" : undefined}
				onClick={onSelect}
				className="flex min-w-0 flex-1 flex-col items-start gap-0.5 px-3 py-2 text-left"
			>
				<span
					className={cn(
						"w-full truncate text-sm",
						active ? "font-medium text-ink" : "text-ink-secondary",
					)}
				>
					{p.name}
				</span>
				<span className="text-2xs text-ink-muted">
					{p.n_positions} position{p.n_positions === 1 ? "" : "s"}
					{p.updated_at && ` · edited ${formatRelative(asUtc(p.updated_at))}`}
				</span>
			</button>
			{/* Hover-revealed on a desk screen; always shown on touch widths. */}
			<div className="flex shrink-0 gap-0.5 pr-1.5 transition-opacity lg:opacity-0 lg:group-focus-within:opacity-100 lg:group-hover:opacity-100">
				<button
					type="button"
					aria-label={`Rename ${p.name}`}
					className="rounded p-1 text-ink-muted hover:text-ink"
					onClick={() => {
						setName(p.name);
						setEditing(true);
					}}
				>
					<Pencil className="size-3.5" />
				</button>
				<button
					type="button"
					aria-label={`Delete ${p.name}`}
					className="rounded p-1 text-ink-muted hover:text-critical"
					onClick={onDelete}
				>
					<Trash2 className="size-3.5" />
				</button>
			</div>
		</li>
	);
}
