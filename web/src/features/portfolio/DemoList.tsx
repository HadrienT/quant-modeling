import { Sparkles } from "lucide-react";
import type { DemoPortfolio } from "@/shared/api";
import { cn } from "@/shared/ui";

/** The demo portfolios: read-only, open one to see it valued live. */
export function DemoList({
	demos,
	activeId,
	isLoading,
	onSelect,
}: {
	demos: DemoPortfolio[];
	activeId: string | null;
	isLoading: boolean;
	onSelect: (id: string) => void;
}) {
	if (isLoading) {
		return <p className="px-1 text-xs text-ink-muted">Pricing the demos…</p>;
	}
	if (!demos.length) {
		return (
			<p className="px-1 text-xs text-ink-muted">
				No demo available: the market data store is unreachable.
			</p>
		);
	}
	return (
		<ul className="flex flex-col gap-1">
			{demos.map(({ portfolio: p, description }) => {
				const active = p.id === activeId;
				return (
					<li key={p.id}>
						<button
							type="button"
							aria-current={active ? "page" : undefined}
							onClick={() => onSelect(p.id)}
							className={cn(
								"relative flex w-full flex-col gap-1 rounded-md border px-3 py-2 text-left transition-colors",
								active
									? "border-accent bg-[color-mix(in_srgb,var(--color-accent)_10%,transparent)]"
									: "border-transparent hover:border-hairline hover:bg-surface-raised",
							)}
						>
							<span
								className={cn(
									"flex items-center gap-1.5 text-sm",
									active ? "font-medium text-ink" : "text-ink-secondary",
								)}
							>
								<Sparkles className="size-3.5 shrink-0 text-accent" />
								{p.name}
								<span className="ml-auto text-2xs text-ink-muted">
									{p.base_currency}
								</span>
							</span>
							<span className="line-clamp-2 text-2xs text-ink-muted">
								{description}
							</span>
						</button>
					</li>
				);
			})}
		</ul>
	);
}
