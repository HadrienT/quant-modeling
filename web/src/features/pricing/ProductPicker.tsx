import {
	CATALOG,
	CATEGORY_LABELS,
	type ProductCategory,
	ProductInfo,
} from "@/shared/products";
import { cn } from "@/shared/ui";
import { Tooltip, TooltipContent, TooltipTrigger } from "@/shared/ui";

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
					{CATALOG.filter((p) => p.category === cat).map((p) => {
						const btn = (
							<button
								key={p.key}
								type="button"
								disabled={!p.enabled}
								onClick={() => onSelect(p.key)}
								className={cn(
									"flex items-center justify-between rounded-sm px-2 py-1 text-left text-sm",
									p.enabled
										? "text-ink-secondary hover:bg-surface-raised hover:text-ink"
										: "cursor-not-allowed text-ink-muted opacity-60",
									selected === p.key && "bg-surface-raised !text-ink",
								)}
							>
								<span>{p.label}</span>
								{p.enabled && <ProductInfo docKey={p.docKey} />}
							</button>
						);
						return p.enabled ? (
							btn
						) : (
							<Tooltip key={p.key}>
								<TooltipTrigger asChild>
									<span>{btn}</span>
								</TooltipTrigger>
								<TooltipContent>{p.disabledReason}</TooltipContent>
							</Tooltip>
						);
					})}
				</div>
			))}
		</nav>
	);
}
