import type { BacktestResponse } from "@/shared/api";
import { Metric, MetricRow } from "@/shared/ui";
import { AllocationChart, EquityAndDrawdownChart } from "@/shared/viz";

export function Results({
	result,
	optWindow,
	pinned,
}: {
	result: BacktestResponse;
	optWindow: { opt_start: string; opt_end: string };
	pinned: { label: string; data: BacktestResponse }[];
}) {
	const m = result.metrics;
	const fmt = (v: number | null | undefined, suffix = "") =>
		v == null ? "n/d" : `${v.toFixed(2)}${suffix}`;

	return (
		<div className="flex flex-col gap-4">
			{result.warnings.length > 0 && (
				<div className="rounded-md border border-warning/40 bg-warning/5 p-3">
					<p className="text-xs font-medium text-warning">Warnings</p>
					<ul className="mt-1 list-disc pl-4 text-xs text-ink-secondary">
						{result.warnings.map((w, i) => (
							<li key={i}>{w}</li>
						))}
					</ul>
				</div>
			)}

			<p className="text-2xs text-ink-muted">
				The grey band is the in-sample optimisation window (
				{optWindow.opt_start} → {optWindow.opt_end}). Not modelled: transaction
				costs, slippage, survivorship bias, dividend treatment. Sharpe uses the
				API's risk-free assumption and √252 annualisation.
			</p>

			<EquityAndDrawdownChart
				portfolio={result.portfolio_values.map((p) => ({
					time: p.date,
					value: p.value,
				}))}
				benchmark={result.sp500_values?.map((p) => ({
					time: p.date,
					value: p.value,
				}))}
				optWindow={{ start: optWindow.opt_start, end: optWindow.opt_end }}
			/>

			<MetricRow>
				<Metric label="Total return" value={fmt(m.total_return_pct, "%")} />
				<Metric label="Annualised" value={fmt(m.annualized_return_pct, "%")} />
				<Metric label="Max drawdown" value={fmt(m.max_drawdown * 100, "%")} />
				<Metric
					label="Sharpe (realised)"
					value={fmt(m.sharpe_ratio)}
					footnote={`in-sample optimum ${fmt(m.optimal_sharpe)}`}
				/>
				<Metric
					label="Alpha / Beta"
					value={`${fmt(m.alpha)} / ${fmt(m.beta)}`}
					footnote={m.alpha == null ? "not computable" : undefined}
				/>
			</MetricRow>

			<AllocationChart
				title="Optimised allocation"
				rows={result.allocation.map((a) => ({
					label: a.ticker,
					weight: a.weight,
					returnPct: a.return_pct,
				}))}
			/>

			{pinned.length > 0 && (
				<div className="text-2xs text-ink-muted">
					{pinned.length} run(s) pinned — overlay comparison shows parameter
					sensitivity.
				</div>
			)}
		</div>
	);
}
