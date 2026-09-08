import {
	CATALOG,
	CATEGORY_LABELS,
	type ProductCategory,
	ProductInfo,
} from "@/shared/products";
import { Tooltip, TooltipContent, TooltipTrigger, cn } from "@/shared/ui";

/** Categorised product list; disabled products stay visible & greyed with a reason (WP 07 §1). */
export function ProductPicker({
	selected,
	onSelect,
}: {
	selected: string;
	onSelect: (key: string) => void;
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
					{CATALOG.filter((p) => p.category === cat).map((p) => (
						// row: the select button and the ⓘ popover are SIBLINGS, never nested
						<div
							key={p.key}
							className={cn(
								"flex items-center gap-1 rounded-sm px-2 py-1 text-sm",
								selected === p.key && "bg-surface-raised",
							)}
						>
							{p.enabled ? (
								<button
									type="button"
									onClick={() => onSelect(p.key)}
									className={cn(
										"flex-1 text-left text-ink-secondary hover:text-ink",
										selected === p.key && "text-ink",
									)}
								>
									{p.label}
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
							<ProductInfo docKey={p.docKey} />
						</div>
					))}
				</div>
			))}
		</nav>
	);
}
