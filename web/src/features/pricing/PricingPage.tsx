import { useState } from "react";
import { Link } from "@tanstack/react-router";
import { BookText, Copy, GitCompareArrows } from "lucide-react";
import { CATALOG_BY_KEY, ParamForm, ProductPicker } from "@/shared/products";
import { Button, Disclaimer, cn, copyText, toast } from "@/shared/ui";
import { useNavPages } from "@/shared/session";
import { AddToPortfolio } from "./AddToPortfolio";
import { ComputeDevicePicker } from "./ComputeDevicePicker";
import { RatesForm } from "./rates/RatesForm";
import { RatesResults } from "./rates/RatesResults";
import { ResultsPanel } from "./ResultsPanel";
import { ScriptedPanel } from "./ScriptedPanel";
import { useWorkbench } from "./useWorkbench";
import { WorkbenchDiagnostics } from "./WorkbenchDiagnostics";

/** The pricing workbench (WP 07). Files in this feature stay under ~200 lines. */
export default function PricingPage() {
	const wb = useWorkbench();
	const { isShown } = useNavPages();
	const [showCompare, setShowCompare] = useState(!!wb.compare);
	// A rates product is priced on a quote set: its own form and results.
	const rates = wb.descriptor.rates;

	return (
		<div className="mx-auto grid max-w-[1400px] grid-cols-1 gap-5 lg:grid-cols-[200px_minmax(320px,380px)_1fr]">
			<aside className="lg:sticky lg:top-16 lg:self-start">
				<ProductPicker selected={wb.productKey} onSelect={wb.setProduct} />
			</aside>

			<section className="flex flex-col gap-4">
				<header className="flex items-center gap-2">
					<h1 className="text-lg font-semibold text-ink">
						{wb.descriptor.label}
					</h1>
					<span className="ml-auto flex items-center gap-3">
						<AddToPortfolio productKey={wb.productKey} values={wb.values} />
						{isShown("/products") && (
							<Link
								to="/products"
								search={{ product: wb.productKey }}
								title="Full reference: payoff, assumptions, sources"
								className="inline-flex items-center gap-1 text-2xs text-ink-muted transition-colors hover:text-accent"
							>
								<BookText className="size-3.5" />
								Reference
							</Link>
						)}
					</span>
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

				{wb.descriptor.gpu && (
					<ComputeDevicePicker
						device={wb.engine === "mc" ? wb.device : "cpu"}
						onChange={wb.setDevice}
					/>
				)}
				{rates ? (
					<RatesForm kind={rates} values={wb.values} onChange={wb.setValues} />
				) : (
					<ParamForm
						key={`${wb.productKey}-${wb.engine}`}
						descriptor={wb.descriptor}
						values={wb.values}
						onChange={wb.setValues}
						hide={hiddenEngineFields(wb.engine)}
					/>
				)}

				<div className="flex gap-2">
					<Button
						size="sm"
						variant="secondary"
						title="Copy a link that reopens this workbench with the same product, engine and inputs"
						onClick={() => {
							void copyText(window.location.href).then((ok) =>
								ok
									? toast.success("Link copied — it reopens this exact setup")
									: toast.error(
											"Couldn't reach the clipboard (needs HTTPS or localhost) — copy the URL from the address bar",
										),
							);
						}}
					>
						<Copy className="size-3.5" /> Copy link
					</Button>
					<Button
						size="sm"
						variant="ghost"
						aria-pressed={showCompare}
						title="Freeze the current inputs as scenario B, then keep editing to see how scenario A moves against it"
						onClick={() => {
							const next = !showCompare;
							setShowCompare(next);
							wb.setCompare(next ? wb.values : null);
						}}
					>
						<GitCompareArrows className="size-3.5" /> A/B compare
					</Button>
					{showCompare && (
						<Button
							size="sm"
							variant="ghost"
							title="Re-freeze scenario B to the inputs shown now"
							onClick={() => wb.setCompare(wb.values)}
						>
							Snapshot B
						</Button>
					)}
				</div>
				{showCompare && (
					<p className="text-2xs text-ink-muted">
						Scenario <span className="text-ink-secondary">B</span> is frozen at
						the inputs from when you enabled compare. Edit the form to move{" "}
						<span className="text-ink-secondary">A</span>; both are priced with
						the same engine so you read off the effect of the change.
					</p>
				)}
			</section>

			<section className="flex flex-col gap-4">
				<div className={cn(showCompare && "grid gap-4 xl:grid-cols-2")}>
					<div>
						<h2 className="mb-2 text-xs font-semibold text-ink-muted uppercase">
							{showCompare ? "A · live · " : ""}
							{rates ? "valuation" : wb.engine}
						</h2>
						{rates ? (
							<RatesResults kind={rates} values={wb.values} />
						) : (
							<ResultsPanel
								descriptor={wb.descriptor}
								values={wb.values}
								engine={wb.engine}
								device={wb.device}
							/>
						)}
						{wb.descriptor.scripted && (
							<div className="mt-4">
								<ScriptedPanel
									descriptor={wb.descriptor}
									values={wb.values}
									engine={wb.engine}
									device={wb.device}
								/>
							</div>
						)}
						{!rates && (
							<div className="mt-4">
								<WorkbenchDiagnostics
									descriptor={wb.descriptor}
									values={wb.values}
									engine={wb.engine}
									device={wb.device}
									open={wb.diagnostics}
									onOpenChange={wb.setDiagnostics}
								/>
							</div>
						)}
					</div>
					{showCompare && wb.compare && (
						<div>
							<h2 className="mb-2 text-xs font-semibold text-ink-muted uppercase">
								B · frozen
							</h2>
							{rates ? (
								<RatesResults kind={rates} values={wb.compare} />
							) : (
								<ResultsPanel
									descriptor={
										CATALOG_BY_KEY.get(wb.productKey) ?? wb.descriptor
									}
									values={wb.compare}
									engine={wb.engine}
									device={wb.device}
								/>
							)}
						</div>
					)}
				</div>
				<Disclaimer />
			</section>
		</div>
	);
}

/** Fields that only some engines read: the tabs above pick the engine, so a
 * second engine selector, or paths for a closed form, would only mislead. */
const ENGINE_ONLY: Record<string, readonly string[]> = {
	n_paths: ["mc"],
	seed: ["mc"],
};

function hiddenEngineFields(engine: string): string[] {
	return [
		"engine",
		...Object.entries(ENGINE_ONLY)
			.filter(([, engines]) => !engines.includes(engine))
			.map(([name]) => name),
	];
}
