import { useQuery } from "@tanstack/react-query";
import { api, ApiError } from "@/shared/api";
import type { ScriptedProduct } from "@/shared/products";

const MODEL_NAMES: Record<string, string> = {
	black_scholes: "Black-Scholes (flat vol)",
	local_vol: "Local vol (Dupire)",
	heston: "Heston",
	slv: "Stochastic-local vol",
};

/** The day before the script's first date: every event is then in the
 * future, whatever today is. */
function beforeFirstDate(script: string): string {
	const first = script.match(/\d{4}-\d{2}-\d{2}/)?.[0] ?? "2027-01-04";
	const d = new Date(`${first}T00:00:00Z`);
	d.setUTCDate(d.getUTCDate() - 1);
	return d.toISOString().slice(0, 10);
}

/**
 * The reference sheet of a product of the script library: its term sheet,
 * the model the page picks for it (read off the script by the real parser,
 * scripting/model_advice.hpp) and the script itself.
 */
export function ScriptedReference({
	product: p,
	withSources,
}: {
	product: ScriptedProduct;
	withSources: boolean;
}) {
	const rec = useQuery({
		queryKey: ["scripted-recommendation", p.slug],
		staleTime: Infinity,
		queryFn: async () => {
			const { data, error } = await api.POST("/price/scripted/validate", {
				body: {
					script: p.script,
					valuation_date: beforeFirstDate(p.script),
					day_count: "ACT/365F",
				},
			});
			if (error !== undefined) throw ApiError.from(error);
			return data;
		},
	});
	const r = rec.data?.recommendation;

	return (
		<>
			<Block title="Model the pricing page picks">
				{r ? (
					<div className="rounded-md border border-hairline bg-surface p-3 text-sm text-ink-secondary">
						<span className="font-medium text-ink">
							{MODEL_NAMES[r.model] ?? r.model}
						</span>
						<p className="mt-1 leading-relaxed">{r.reason}</p>
					</div>
				) : (
					<p className="text-sm text-ink-muted">
						{rec.error ? rec.error.message : "Reading the script…"}
					</p>
				)}
			</Block>

			<Block title="Term sheet">
				{p.params.length === 0 ? (
					<p className="text-sm text-ink-muted">
						No terms: the payoff is fixed.
					</p>
				) : (
					<table className="w-full rounded-md border border-hairline bg-surface text-sm">
						<tbody>
							{p.params.map((prm) => (
								<tr
									key={prm.name}
									className="border-t border-hairline first:border-0"
								>
									<td className="p-2 text-ink-secondary">{prm.label}</td>
									<td className="p-2 font-mono text-2xs text-ink-muted">
										{prm.name}
									</td>
									<td className="p-2 text-right font-mono text-ink tabular-nums">
										{prm.unit === "percent"
											? `${+(prm.default * 100).toFixed(4)}%`
											: prm.default}
									</td>
								</tr>
							))}
						</tbody>
					</table>
				)}
			</Block>

			<Block title="Payoff script">
				<pre className="overflow-x-auto rounded-md border border-hairline bg-surface p-3 font-mono text-2xs text-ink-secondary">
					{p.script}
				</pre>
			</Block>

			{withSources && (
				<Block title="References">
					<ol className="flex list-decimal flex-col gap-2 pl-5 text-sm text-ink-secondary">
						{p.sources.map((s) => (
							<li key={s}>{s}</li>
						))}
					</ol>
				</Block>
			)}
		</>
	);
}

function Block({
	title,
	children,
}: {
	title: string;
	children: React.ReactNode;
}) {
	return (
		<section className="flex flex-col gap-3">
			<h2 className="text-2xs font-semibold tracking-wide text-ink-muted uppercase">
				{title}
			</h2>
			<div>{children}</div>
		</section>
	);
}
