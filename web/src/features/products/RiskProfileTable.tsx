import type { ProductDescriptor } from "@/shared/products";
import { formatNumber } from "@/shared/format";
import { Badge } from "@/shared/ui";
import {
	type RiskProfile,
	type RiskRow,
	useNativeRiskProfile,
	useScriptedRiskProfile,
} from "./riskProfile";

const TONE: Record<RiskRow["position"], "good" | "critical" | "neutral"> = {
	long: "good",
	short: "critical",
	"not significant": "neutral",
	negligible: "neutral",
};

/**
 * Long or short what — the table Bouzoubaa & Osseiran draw up for every
 * structure, computed here by bumping each market parameter and repricing:
 * the holder is long a parameter when the price rises with it.
 */
export function RiskProfileTable({
	descriptor,
}: {
	descriptor: ProductDescriptor;
}) {
	if (descriptor.scripted) return <Scripted slug={descriptor.scripted.slug} />;
	if (!descriptor.enabled || !descriptor.endpoint)
		return (
			<p className="text-sm text-ink-muted">
				Not computed: this product has no pricing engine to bump.
			</p>
		);
	return <Native descriptor={descriptor} />;
}

function Scripted({ slug }: { slug: string }) {
	const q = useScriptedRiskProfile(slug);
	return <Body data={q.data} error={q.error} loading={q.isLoading} />;
}

function Native({ descriptor }: { descriptor: ProductDescriptor }) {
	const q = useNativeRiskProfile(descriptor);
	return <Body data={q.data} error={q.error} loading={q.isLoading} />;
}

function Body({
	data,
	error,
	loading,
}: {
	data?: RiskProfile;
	error: Error | null;
	loading: boolean;
}) {
	if (loading)
		return (
			<p className="text-sm text-ink-muted">
				Bumping every parameter and repricing…
			</p>
		);
	if (error) return <p className="text-sm text-critical">{error.message}</p>;
	if (!data) return null;
	return (
		<div className="flex flex-col gap-2">
			<div className="overflow-x-auto rounded-md border border-hairline bg-surface">
				<table className="w-full text-sm">
					<thead>
						<tr className="text-2xs text-ink-muted uppercase">
							<th className="p-2 text-left">Parameter</th>
							<th className="p-2 text-left">Holder is</th>
							<th className="p-2 text-right">Price change</th>
							<th className="p-2 text-left">Bump</th>
						</tr>
					</thead>
					<tbody>
						{data.rows.map((r) => (
							<tr key={r.factor} className="border-t border-hairline">
								<td className="p-2 text-ink">{r.factor}</td>
								<td className="p-2">
									<Badge tone={TONE[r.position]}>{r.position}</Badge>
								</td>
								<td className="p-2 text-right font-mono text-2xs tabular-nums">
									{formatNumber(r.change, "greek")}
									{r.std_error > 0 && (
										<span className="text-ink-muted">
											{" "}
											± {formatNumber(r.std_error, "greek")}
										</span>
									)}
								</td>
								<td className="p-2 text-2xs text-ink-secondary">{r.bump}</td>
							</tr>
						))}
					</tbody>
				</table>
			</div>
			<p className="text-2xs text-ink-muted">
				Computed on {data.reference}. {data.method} A change within two standard
				errors is “not significant”, one below 0.01% of the price “negligible”.
			</p>
		</div>
	);
}
