import { cn } from "@/shared/ui";
import { CATALOG, CATEGORY_LABELS } from "./catalog";
import type { ProductCategory } from "./types";

/**
 * Product list by family (WP 07 §1), hand-written pricers and scripts mixed.
 * Two consumers:
 *   - the pricing workbench, which lists only what it can price;
 *   - the Products reference page (`allowDisabled`), which also lists the
 *     products it documents without a pricing yet, marked as such.
 * A product's documentation is its sheet on the Products page, not a popover.
 */
export function ProductPicker({
	selected,
	onSelect,
	allowDisabled = false,
}: {
	selected: string;
	onSelect: (key: string) => void;
	allowDisabled?: boolean;
}) {
	const listed = CATALOG.filter((p) => p.enabled || allowDisabled);
	const cats = [...new Set(listed.map((p) => p.category))] as ProductCategory[];

	return (
		<nav className="flex flex-col gap-3">
			{cats.map((cat) => (
				<div key={cat} className="flex flex-col gap-0.5">
					<span className="px-1 text-2xs font-semibold tracking-wide text-ink-muted uppercase">
						{CATEGORY_LABELS[cat]}
					</span>
					{listed
						.filter((p) => p.category === cat)
						.map((p) => {
							const active = selected === p.key;
							return (
								<button
									key={p.key}
									type="button"
									onClick={() => onSelect(p.key)}
									aria-current={active ? "true" : undefined}
									className={cn(
										"rounded-sm px-2 py-1 text-left text-sm hover:text-ink",
										p.enabled ? "text-ink-secondary" : "text-ink-muted",
										active && "bg-surface-raised !text-ink",
									)}
								>
									{p.label}
									{!p.enabled && (
										<span className="ml-1 text-2xs text-ink-muted">
											· no pricing yet
										</span>
									)}
								</button>
							);
						})}
				</div>
			))}
		</nav>
	);
}
