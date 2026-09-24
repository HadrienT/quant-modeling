import { Copy, Sparkles } from "lucide-react";
import { Button } from "@/shared/ui";

/** Says a demo is read-only and how its trades were priced; offers a copy. */
export function DemoBanner({
	description,
	onCopy,
}: {
	description: string;
	onCopy: () => void;
}) {
	return (
		<div className="flex flex-wrap items-start justify-between gap-3 rounded-md border border-accent/40 bg-[color-mix(in_srgb,var(--color-accent)_8%,transparent)] px-4 py-3">
			<div className="flex min-w-0 flex-1 flex-col gap-1">
				<span className="flex items-center gap-1.5 text-xs font-semibold tracking-wide text-accent uppercase">
					<Sparkles className="size-3.5" /> Demo portfolio · read-only
				</span>
				<p className="text-sm text-ink-secondary">{description}</p>
				<p className="text-2xs text-ink-muted">
					Every trade is priced from the stored market on its date — a stock at
					that day&apos;s close, an option at its model value that day — so the
					P&L is the one these trades really made.
				</p>
			</div>
			<Button size="sm" onClick={onCopy}>
				<Copy className="size-3.5" /> Copy to my portfolios
			</Button>
		</div>
	);
}
