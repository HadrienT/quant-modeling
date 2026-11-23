import { useMemo } from "react";
import { type ComputeDevice, usePricing } from "@/shared/api";
import type { ProductDescriptor } from "@/shared/products";
import { ModelDecision } from "./scripting/ModelDecision";
import { ModelWarnings, TIME_LIMIT } from "./scripting/ModelWarnings";
import { pricingBody } from "./requestBody";

/**
 * What the pricing workbench adds for a product of the script library: the
 * model it was priced under (and why), what the model cannot capture, the
 * script actually priced. The same terms under every dynamics, and the vega
 * to market quotes, are in the Diagnostics section (WP 28, lot U3). Reads the
 * same query as ResultsPanel (same key, one request).
 */
export function ScriptedPanel({
	descriptor,
	values,
	engine,
	device = "cpu",
}: {
	descriptor: ProductDescriptor;
	values: Record<string, unknown>;
	engine: string;
	device?: ComputeDevice;
}) {
	const body = useMemo(
		() => pricingBody(descriptor, values, engine, device),
		[descriptor, values, engine, device],
	);
	const q = usePricing({ endpoint: descriptor.endpoint ?? null, body });
	const r = q.data;

	return (
		<div className="flex flex-col gap-3">
			{r?.model_choice && <ModelDecision choice={r.model_choice} />}
			{r && <ModelWarnings warnings={r.warnings ?? []} skip={TIME_LIMIT} />}
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
		</div>
	);
}
