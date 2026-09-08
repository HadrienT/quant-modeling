import { useMemo } from "react";
import type { ProductDescriptor } from "@/shared/products";
import type { PricingResponse } from "@/shared/api";
import { usePricing } from "@/shared/api";
import {
	EngineTag,
	Freshness,
	NumberCell,
	Provenance,
	Uncertainty,
} from "@/shared/ui";
import { ErrorState, MetricRowSkeleton } from "@/shared/ui/states";
import { ConvergenceChart, GreekProfileChart, PayoffChart } from "@/shared/viz";
import { blackScholes } from "@/shared/payoff";

const GREEKS = ["delta", "gamma", "vega", "theta", "rho"] as const;

/**
 * The panel that carries the screen's credibility (WP 07 §4): price WITH its
 * standard error, greeks with theirs, EngineTag / Provenance / Freshness,
 * diagnostics shown, bond analytics, MC convergence, payoff + greek profiles.
 */
export function ResultsPanel({
	descriptor,
	values,
	engine,
}: {
	descriptor: ProductDescriptor;
	values: Record<string, unknown>;
	engine: string;
}) {
	const body = useMemo(
		() => descriptor.toRequest(values, engine as never),
		[descriptor, values, engine],
	);
	const q = usePricing({ endpoint: descriptor.endpoint, body });

	const isMc = engine === "mc";

	// local BS payoff / greek profiles for the vanilla-family products
	const profile = useMemo(() => {
		if (!["vanilla", "american", "asian", "digital"].includes(descriptor.key))
			return null;
		const S0 = Number(values.spot) || 100;
		const spots = Array.from(
			{ length: 61 },
			(_, i) => S0 * 0.6 + (S0 * 0.8 * i) / 60,
		);
		const base = {
			strike: Number(values.strike) || S0,
			rate: (Number(values.rate) || 0) / 100,
			dividend: (Number(values.dividend) || 0) / 100,
			vol: (Number(values.vol) || 20) / 100,
			maturity: Number(values.maturity) || 1,
			isCall: values.is_call !== false,
		};
		const payoff = spots.map((S) =>
			Math.max(0, base.isCall ? S - base.strike : base.strike - S),
		);
		const atT = spots.map((S) => blackScholes({ ...base, spot: S }).price);
		const greeks = (["delta", "gamma", "vega", "theta"] as const).map((g) => ({
			name: g[0]!.toUpperCase() + g.slice(1),
			spot: spots,
			values: spots.map((S) => blackScholes({ ...base, spot: S })[g]),
		}));
		return { spots, payoff, atT, greeks, S0 };
	}, [descriptor.key, values]);

	if (q.isLoading) return <MetricRowSkeleton />;
	if (q.error)
		return <ErrorState error={q.error} onRetry={() => q.refetch()} />;
	if (!q.data) return null;

	const r = q.data as PricingResponse;
	const g = r.greeks;
	const se = (name: string) =>
		(g as Record<string, number | null | undefined>)[`${name}_std_error`];

	return (
		<div className="flex flex-col gap-4">
			<div className="rounded-md border border-hairline bg-surface p-4">
				<div className="flex items-baseline justify-between gap-4">
					<div className="flex flex-col gap-1">
						<span className="text-2xs text-ink-muted uppercase">
							Present value
						</span>
						<span className="text-xl">
							<Uncertainty
								value={r.npv}
								stdError={isMc ? r.mc_std_error : null}
								magnitude="price"
							/>
						</span>
					</div>
					<div className="flex flex-col items-end gap-1">
						<EngineTag engine={engine} />
						<Provenance source="manual" />
						<Freshness at={Date.now()} />
					</div>
				</div>
				{r.diagnostics && (
					<p className="mt-2 font-mono text-2xs text-ink-muted">
						{r.diagnostics}
					</p>
				)}
			</div>

			<table className="w-full rounded-md border border-hairline bg-surface text-sm">
				<thead>
					<tr className="text-2xs text-ink-muted uppercase">
						<th className="p-2 text-left">Greek</th>
						<th className="p-2 text-right">Value</th>
						<th className="p-2 text-right">Std error</th>
					</tr>
				</thead>
				<tbody className="font-mono tabular-nums">
					{GREEKS.map((name) => {
						const v = (g as Record<string, number | null | undefined>)[name];
						if (v == null) return null;
						return (
							<tr key={name} className="border-t border-hairline">
								<td className="p-2 text-left text-ink-secondary">{name}</td>
								<td className="p-2 text-right">
									<NumberCell value={v} magnitude="greek" />
								</td>
								<td className="p-2 text-right text-ink-muted">
									{se(name) != null ? (
										<NumberCell value={se(name)} magnitude="greek" />
									) : (
										"—"
									)}
								</td>
							</tr>
						);
					})}
				</tbody>
			</table>

			{r.bond_analytics && (
				<div className="grid grid-cols-2 gap-3 rounded-md border border-hairline bg-surface p-4 text-sm sm:grid-cols-4">
					{Object.entries(r.bond_analytics).map(([k, v]) => (
						<div key={k} className="flex flex-col">
							<span className="text-2xs text-ink-muted uppercase">
								{k.replace(/_/g, " ")}
							</span>
							<NumberCell value={v as number} magnitude="plain" />
						</div>
					))}
				</div>
			)}

			{isMc && (
				<ConvergenceChart
					title="Convergence (reference 1/√N)"
					series={[
						{
							label: "this run",
							points: [0.1, 0.25, 0.5, 1].map((frac) => ({
								paths: Math.round(
									Number(body ? (values.n_paths ?? 200000) : 200000) * frac,
								),
								error: r.mc_std_error / Math.sqrt(frac),
							})),
						},
					]}
				/>
			)}

			{profile && (
				<>
					<PayoffChart
						title="Payoff & value at t"
						data={{
							spot: profile.spots,
							atMaturity: profile.payoff,
							atT: profile.atT,
							currentSpot: profile.S0,
							strikes: [Number(values.strike) || profile.S0],
						}}
					/>
					<GreekProfileChart
						currentSpot={profile.S0}
						profiles={profile.greeks}
					/>
				</>
			)}
		</div>
	);
}
