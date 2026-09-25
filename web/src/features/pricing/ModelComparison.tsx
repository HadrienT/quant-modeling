import { usePricing } from "@/shared/api";
import { formatNumber } from "@/shared/format";
import { scriptedRequest, type ScriptedProduct } from "@/shared/products";
import { MODEL_LABELS } from "./scripting/modelLabels";

const MODELS = ["black_scholes", "local_vol", "heston", "slv"] as const;
type Model = (typeof MODELS)[number];

const WHAT: Record<Model, string> = {
	black_scholes: "one flat vol: the at-the-money implied vol at maturity",
	local_vol: "today's whole smile, exactly; a flat forward smile",
	heston: "its own smile dynamics; today's smile only up to its fit error",
	slv: "today's smile exactly, and Heston's forward smile",
};

/**
 * The same term sheet priced under every dynamics, on one seed: what a more
 * complete model changes. A difference is flagged only when it exceeds two
 * combined Monte-Carlo standard errors.
 */
export function ModelComparison({
	product,
	values,
}: {
	product: ScriptedProduct;
	values: Record<string, unknown>;
}) {
	// One hook per model, in a fixed order (no hooks inside a loop).
	const bs = useModelPrice(product, values, "black_scholes");
	const lv = useModelPrice(product, values, "local_vol");
	const heston = useModelPrice(product, values, "heston");
	const slv = useModelPrice(product, values, "slv");
	const rows = [bs, lv, heston, slv].map((q, i) => ({ model: MODELS[i]!, q }));
	const base = rows[0]!.q.data;
	return (
		<div className="overflow-x-auto rounded-md border border-hairline bg-surface">
			<table className="w-full text-sm">
				<thead>
					<tr className="text-2xs text-ink-muted uppercase">
						<th className="p-2 text-left">Dynamics</th>
						<th className="p-2 text-right">Price</th>
						<th className="p-2 text-right">vs Black-Scholes</th>
						<th className="p-2 text-left">What it captures</th>
					</tr>
				</thead>
				<tbody>
					{rows.map(({ model, q }) => {
						const r = q.data;
						const diff = r && base ? r.npv - base.npv : null;
						const se =
							r && base ? Math.hypot(r.mc_std_error, base.mc_std_error) : null;
						const significant =
							diff != null && se != null && model !== "black_scholes"
								? Math.abs(diff) > 2 * se
								: false;
						return (
							<tr key={model} className="border-t border-hairline align-top">
								<td className="p-2 text-ink">{MODEL_LABELS[model]}</td>
								<td className="p-2 text-right font-mono tabular-nums">
									{q.isLoading
										? "…"
										: q.error
											? "failed"
											: r
												? `${formatNumber(r.npv, "price")} ± ${formatNumber(r.mc_std_error, "price")}`
												: "—"}
								</td>
								<td className="p-2 text-right font-mono tabular-nums">
									{model === "black_scholes" || diff == null
										? "—"
										: `${diff >= 0 ? "+" : "−"}${formatNumber(Math.abs(diff), "price")}${significant ? "" : " (noise)"}`}
								</td>
								<td className="p-2 text-2xs text-ink-secondary">
									{q.error ? q.error.message : WHAT[model]}
								</td>
							</tr>
						);
					})}
				</tbody>
			</table>
			<p className="border-t border-hairline p-2 text-2xs text-ink-muted">
				Same terms, market and seed under each model. “(noise)”: the difference
				is within two combined Monte-Carlo standard errors.
			</p>
		</div>
	);
}

function useModelPrice(
	product: ScriptedProduct,
	values: Record<string, unknown>,
	model: Model,
) {
	return usePricing({
		endpoint: "/price/scripted-product",
		body: scriptedRequest(product, values, model),
	});
}
