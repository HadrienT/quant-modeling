import { useState } from "react";
import { Copy, GitCompareArrows } from "lucide-react";
import { CATALOG_BY_KEY, ProductInfo } from "@/shared/products";
import { Button, cn, toast } from "@/shared/ui";
import { ParamForm } from "@/shared/products";
import { ProductPicker } from "./ProductPicker";
import { ResultsPanel } from "./ResultsPanel";
import { useWorkbench } from "./useWorkbench";

/** The pricing workbench (WP 07). Files in this feature stay under ~200 lines. */
export default function PricingPage() {
	const wb = useWorkbench();
	const [showCompare, setShowCompare] = useState(!!wb.compare);

	return (
		<div className="mx-auto grid max-w-[1400px] gap-5 lg:grid-cols-[200px_minmax(320px,380px)_1fr]">
			<aside className="lg:sticky lg:top-16 lg:self-start">
				<ProductPicker selected={wb.productKey} onSelect={wb.setProduct} />
			</aside>

			<section className="flex flex-col gap-4">
				<header className="flex items-center gap-2">
					<h1 className="text-lg font-semibold text-ink">
						{wb.descriptor.label}
					</h1>
					<ProductInfo docKey={wb.descriptor.docKey} />
				</header>

				{wb.descriptor.engines.length > 1 && (
					<div className="flex flex-wrap gap-1">
						{wb.descriptor.engines.map((e) => (
							<button
								key={e.key}
								type="button"
								onClick={() => wb.setEngine(e.key)}
								className={cn(
									"rounded-sm border px-2 py-1 text-xs",
									wb.engine === e.key
										? "border-accent bg-accent/10 text-ink"
										: "border-hairline text-ink-secondary hover:text-ink",
								)}
							>
								{e.label}
							</button>
						))}
					</div>
				)}

				<ParamForm
					key={`${wb.productKey}-${wb.engine}`}
					descriptor={wb.descriptor}
					values={wb.values}
					onChange={wb.setValues}
				/>

				<div className="flex gap-2">
					<Button
						size="sm"
						variant="secondary"
						onClick={() => {
							void navigator.clipboard?.writeText(window.location.href);
							toast.success("Pricing link copied");
						}}
					>
						<Copy className="size-3.5" /> Copy link
					</Button>
					<Button
						size="sm"
						variant="ghost"
						aria-pressed={showCompare}
						onClick={() => {
							const next = !showCompare;
							setShowCompare(next);
							wb.setCompare(next ? wb.values : null);
						}}
					>
						<GitCompareArrows className="size-3.5" /> A/B compare
					</Button>
				</div>
			</section>

			<section className="flex flex-col gap-4">
				<div className={cn(showCompare && "grid gap-4 xl:grid-cols-2")}>
					<div>
						<h2 className="mb-2 text-xs font-semibold text-ink-muted uppercase">
							A · {wb.engine}
						</h2>
						<ResultsPanel
							descriptor={wb.descriptor}
							values={wb.values}
							engine={wb.engine}
						/>
					</div>
					{showCompare && wb.compare && (
						<div>
							<h2 className="mb-2 text-xs font-semibold text-ink-muted uppercase">
								B
							</h2>
							<ResultsPanel
								descriptor={CATALOG_BY_KEY.get(wb.productKey) ?? wb.descriptor}
								values={wb.compare}
								engine={wb.engine}
							/>
						</div>
					)}
				</div>
			</section>
		</div>
	);
}
