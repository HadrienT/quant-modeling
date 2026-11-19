import { useMemo } from "react";
import type { ProductDescriptor } from "@/shared/products";
import { type ComputeDevice, deviceLabel, usePricing } from "@/shared/api";
import {
	ComputeBreakdown,
	ComputeTime,
	DiscretisationBias,
	EngineTag,
	Freshness,
	NumberCell,
	Provenance,
	Uncertainty,
} from "@/shared/ui";
import { ErrorState, MetricRowSkeleton } from "@/shared/ui/states";
import { pricingBody } from "./requestBody";
import { ScriptGreeks } from "./scripting/ScriptGreeks";

const GREEKS = ["delta", "gamma", "vega", "theta", "rho"] as const;

/** Each greek's name and unit, as the adjoint table gives them. */
const GREEK_UNITS: Record<(typeof GREEKS)[number], [string, string]> = {
	delta: ["Delta", "per 1.00 of spot"],
	gamma: ["Gamma", "per 1.00 of spot, squared"],
	vega: ["Vega", "per 1.00 of vol (÷100 for one vol point)"],
	theta: ["Theta", "per year (÷365 for one day)"],
	rho: ["Rho", "per 1.00 of rate (÷10,000 for one bp)"],
};

/** A rates product's: the rate is the underlying, sigma the short rate's. */
const RATES_GREEK_UNITS: Record<(typeof GREEKS)[number], [string, string]> = {
	...GREEK_UNITS,
	gamma: ["Gamma", "per 1.00 of rate, squared (÷10⁸ per bp²)"],
	vega: ["Vega", "per 1.00 of the short rate's σ (÷10,000 for one bp)"],
	rho: ["Rho (DV01 × 10,000)", "per 1.00 of rate (÷10,000 for one bp)"],
};

type Result = NonNullable<ReturnType<typeof usePricing>["data"]> & {
	model_choice?: {
		calibration?: unknown;
		rates_calibration?: unknown;
		underlyings?: { ticker?: string | null }[] | null;
	} | null;
};

/** Whether the price rests on the database's market (a ticker's chain,
 * close and dividend, or the traded rates) rather than typed inputs. */
function fromMarket(r: Result): boolean {
	const c = r.model_choice;
	return !!(
		c?.calibration ||
		c?.rates_calibration ||
		c?.underlyings?.some((u) => u.ticker)
	);
}

/** The underlyings' tickers, spot(0) first, to name their greeks. */
function underlyingNames(r: Result, values: Record<string, unknown>): string[] {
	const used = r.model_choice?.underlyings?.map((u) => u.ticker ?? "");
	if (used?.some(Boolean)) return used as string[];
	// typed underlyings have no name: spot(i)
	if (values.source === "typed") return [];
	return String(values.tickers ?? "")
		.split(/[\s,;]+/)
		.filter(Boolean)
		.map((t) => t.toUpperCase());
}

/**
 * The panel that carries the screen's credibility (WP 07 §4): price WITH its
 * standard error, greeks with theirs, EngineTag / Provenance / Freshness,
 * diagnostics shown, bond analytics. MC convergence is
 * in the Diagnostics section below (WP 28, lot U3).
 */
export function ResultsPanel({
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

	const isMc = engine === "mc";

	if (q.isLoading) return <MetricRowSkeleton />;
	if (q.error)
		return <ErrorState error={q.error} onRetry={() => q.refetch()} />;
	if (!q.data) return null;

	const r = q.data;
	const g = r.greeks;
	const units =
		descriptor.scripted?.market === "rates" ? RATES_GREEK_UNITS : GREEK_UNITS;
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
						<Uncertainty
							value={r.npv}
							stdError={isMc ? r.mc_std_error : null}
							magnitude="price"
							emphasis
						/>
					</div>
					<div className="flex flex-col items-end gap-1">
						<EngineTag engine={engine} />
						{isMc && descriptor.gpu && (
							<EngineTag engine={`on ${deviceLabel(r)}`} />
						)}
						<Provenance
							source={
								values.source !== "typed" &&
								(descriptor.scripted || fromMarket(r))
									? "market"
									: "manual"
							}
						/>
						<Freshness at={Date.now()} />
						<ComputeTime
							computeMs={r.compute_ms}
							roundTripMs={r.round_trip_ms}
						/>
					</div>
				</div>
				<ComputeBreakdown timings={r.timings} engine={engine} />
				{isMc && (
					<DiscretisationBias
						bias={r.discretisation_bias}
						error={r.discretisation_bias_std_error}
						value={r.npv}
					/>
				)}
				{r.diagnostics && (
					<details className="mt-2 text-2xs text-ink-muted">
						<summary className="cursor-pointer select-none">
							Engine details
						</summary>
						<p className="mt-1 font-mono">{r.diagnostics}</p>
					</details>
				)}
			</div>

			{(r.risks ?? []).length > 0 ? (
				<ScriptGreeks
					risks={r.risks ?? []}
					names={underlyingNames(r, values)}
				/>
			) : (
				GREEKS.some((n) => (g as Record<string, unknown>)[n] != null) && (
					<table className="w-full rounded-md border border-hairline bg-surface text-sm">
						<thead>
							<tr className="text-2xs text-ink-muted uppercase">
								<th className="p-2 text-left">Greek</th>
								<th className="p-2 text-right">Value</th>
								<th className="p-2 text-right">Std error</th>
								<th className="p-2 text-left">Unit</th>
							</tr>
						</thead>
						<tbody className="font-mono tabular-nums">
							{GREEKS.map((name) => {
								const v = (g as Record<string, number | null | undefined>)[
									name
								];
								if (v == null) return null;
								return (
									<tr key={name} className="border-t border-hairline">
										<td className="p-2 text-left font-sans text-ink">
											{units[name][0]}
										</td>
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
										<td className="p-2 text-left font-sans text-2xs text-ink-secondary">
											{units[name][1]}
										</td>
									</tr>
								);
							})}
						</tbody>
						{(r.bumped_greeks ?? []).length > 0 && (
							<caption className="caption-bottom p-2 text-left text-2xs text-ink-muted">
								{(r.bumped_greeks ?? []).join(", ")}: not given by this engine,
								computed by bumping the input and revaluing
								{isMc ? " on the same random draws" : ""} (finite differences,
								no standard error). Per unit of spot, volatility, rate; theta
								per year.
							</caption>
						)}
					</table>
				)
			)}

			{r.bond_analytics &&
				Object.values(r.bond_analytics).some((v) => v != null) && (
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
		</div>
	);
}
