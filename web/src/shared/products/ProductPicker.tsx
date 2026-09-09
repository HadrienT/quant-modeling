import { Tooltip, TooltipContent, TooltipTrigger, cn } from "@/shared/ui";
import { CATALOG, CATEGORY_LABELS } from "./catalog";
import { ProductInfo } from "./ProductInfo";
import type { ProductCategory } from "./types";

/**
 * Categorised product list (WP 07 §1). Two consumers:
 *   - the pricing workbench, where a disabled product is greyed and inert and
 *     the ⓘ popover is the quick way to peek at the doc;
 *   - the Products reference page (`allowDisabled`), where every product is
 *     clickable because you can still read about one you cannot price, and the
 *     ⓘ popover is dropped (`showInfo={false}`) — the full sheet is right there.
 * When shown, the ⓘ popover and the row button are always SIBLINGS, never nested.
 */
export function ProductPicker({
	selected,
	onSelect,
	allowDisabled = false,
	showInfo = true,
}: {
	selected: string;
	onSelect: (key: string) => void;
	allowDisabled?: boolean;
	showInfo?: boolean;
}) {
	const cats = [
		...new Set(CATALOG.map((p) => p.category)),
	] as ProductCategory[];

	return (
		<nav className="flex flex-col gap-3">
			{cats.map((cat) => (
				<div key={cat} className="flex flex-col gap-0.5">
					<span className="px-1 text-2xs font-semibold tracking-wide text-ink-muted uppercase">
						{CATEGORY_LABELS[cat]}
					</span>
					{CATALOG.filter((p) => p.category === cat).map((p) => {
						const active = selected === p.key;
						const asButton = p.enabled || allowDisabled;
						return (
							<div
								key={p.key}
								className={cn(
									"flex items-center gap-1 rounded-sm px-2 py-1 text-sm",
									active && "bg-surface-raised",
								)}
							>
								{asButton ? (
									<button
										type="button"
										onClick={() => onSelect(p.key)}
										className={cn(
											"flex-1 text-left hover:text-ink",
											p.enabled ? "text-ink-secondary" : "text-ink-muted",
											active && "!text-ink",
										)}
									>
										{p.label}
										{!p.enabled && (
											<span className="ml-1 text-2xs text-ink-muted">
												· preview
											</span>
										)}
									</button>
								) : (
									<Tooltip>
										<TooltipTrigger asChild>
											<span className="flex-1 cursor-not-allowed text-ink-muted opacity-60">
												{p.label}
											</span>
										</TooltipTrigger>
										<TooltipContent>{p.disabledReason}</TooltipContent>
									</Tooltip>
								)}
								{showInfo && <ProductInfo docKey={p.docKey} />}
							</div>
						);
					})}
				</div>
			))}
		</nav>
	);
}
