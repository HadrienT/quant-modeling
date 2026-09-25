import { useMemo, useState } from "react";
import { usePricing } from "@/shared/api";
import type { ProductDescriptor } from "@/shared/products";
import { Button } from "@/shared/ui";
import { ModelComparison } from "./ModelComparison";
import { ModelDecision } from "./scripting/ModelDecision";
import { ModelWarnings } from "./scripting/ModelWarnings";

/**
 * What the pricing workbench adds for a product of the script library: the
 * model it was priced under (and why), what the model cannot capture, the
 * script actually priced, and — on request — the same terms under every
 * dynamics. Reads the same query as ResultsPanel (same key, one request).
 */
export function ScriptedPanel({
	descriptor,
	values,
	engine,
}: {
	descriptor: ProductDescriptor;
	values: Record<string, unknown>;
	engine: string;
}) {
	const [compare, setCompare] = useState(false);
	const body = useMemo(
		() => descriptor.toRequest(values, engine as never),
		[descriptor, values, engine],
	);
	const q = usePricing({ endpoint: descriptor.endpoint ?? null, body });
	const p = descriptor.scripted!;
	const r = q.data;

	return (
		<div className="flex flex-col gap-3">
			{r?.model_choice && <ModelDecision choice={r.model_choice} />}
			{r && <ModelWarnings warnings={r.warnings ?? []} />}
			{r?.script && (
				<details className="rounded-md border border-hairline bg-surface p-3 text-2xs">
					<summary className="cursor-pointer text-ink-muted">
						Script priced (open it in the scripting page to edit)
					</summary>
					<pre className="mt-2 overflow-x-auto font-mono text-ink-secondary">
						{r.script}
					</pre>
				</details>
			)}
			{p.underlyings === 1 ? (
				<div className="flex flex-col gap-2">
					<Button
						size="sm"
						variant="secondary"
						className="w-fit"
						aria-pressed={compare}
						onClick={() => setCompare((c) => !c)}
					>
						{compare ? "Hide the model comparison" : "Compare the dynamics"}
					</Button>
					{compare && <ModelComparison product={p} values={values} />}
				</div>
			) : (
				<p className="text-2xs text-ink-muted">
					Several underlyings: correlated Black-Scholes is the only multi-asset
					model for scripts, so there is nothing to compare yet.
				</p>
			)}
		</div>
	);
}
