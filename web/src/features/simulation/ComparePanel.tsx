import type { DistributionResponse } from "@/shared/api/types";
import { TickerSelect } from "@/shared/market";
import { Button } from "@/shared/ui";
import { ErrorState } from "@/shared/ui/states";
import { DensityChart, SmileFitChart, useChartTheme } from "@/shared/viz";
import { binCentres, pct, smileSeries } from "./distribution";
import type { UseComparison } from "./useComparison";
import type { UseSimulation } from "./useSimulation";

const NAMES = { heston: "Heston", slv: "Stochastic-local vol" } as const;

/**
 * Same smile today, different dynamics (WP 29, lot S4): local vol and a
 * stochastic-vol model calibrated on one ticker's stored surface, on the
 * same paths count and seed. Their vanilla smiles at the last date both
 * sit on the market's; their forward smiles and their up-and-out prices do
 * not -- the classic argument for why the choice of model matters for
 * anything that depends on the path.
 */
export function ComparePanel({
	sim,
	cmp,
}: {
	sim: UseSimulation;
	cmp: UseComparison;
}) {
	const t = useChartTheme();
	const a = cmp.localVol.data;
	const b = cmp.stochastic.data;
	const otherName = NAMES[cmp.other];
	const colors = [t.series[0]!, t.series[1]!];
	const run = () => {
		// Both calibrated on the ticker: the grid and the effort of the
		// single-model form, none of its parameters (dividend from the database).
		const r = sim.request();
		cmp.run({
			rate: r.rate,
			ttm: r.ttm,
			n_steps: r.n_steps,
			n_paths: r.n_paths,
			seed: r.seed,
			n_display: 0,
			barrier: r.barrier,
			barrier_strike: r.barrier_strike,
			dividend: 0,
			ticker: sim.ticker,
		});
	};

	return (
		<div className="flex flex-col gap-4">
			<div className="flex flex-wrap items-end gap-3 rounded-md border border-hairline bg-surface p-4">
				<div className="flex min-w-72 flex-col gap-1 text-sm">
					<span className="text-2xs text-ink-muted uppercase">Ticker</span>
					<TickerSelect value={sim.ticker} onChange={sim.setTicker} />
				</div>
				<label className="flex flex-col gap-1 text-sm">
					<span className="text-2xs text-ink-muted uppercase">Against</span>
					<select
						className="rounded border border-hairline bg-surface px-2 py-1"
						value={cmp.other}
						onChange={(e) => cmp.setOther(e.target.value as "heston" | "slv")}
					>
						<option value="slv">{NAMES.slv}</option>
						<option value="heston">{NAMES.heston}</option>
					</select>
				</label>
				<Button onClick={run} disabled={cmp.isPending || !sim.ticker}>
					{cmp.isPending ? "Simulating…" : "Compare"}
				</Button>
				<p className="basis-full text-xs text-ink-muted">
					Both calibrated on the stored option chain; maturity, steps, paths and
					seed from the single-model form.
				</p>
			</div>
			{cmp.error && <ErrorState error={cmp.error} />}
			{a && b && (
				<>
					<div className="grid grid-cols-1 gap-3 lg:grid-cols-2">
						<SmileFitChart
							title={`Smile at ${last(a).time.toFixed(2)}y: both on the market`}
							series={[
								...smileSeries(last(a), "Local vol", colors[0], t.inkMuted),
								smileSeries(last(b), otherName, colors[1])[0]!,
							]}
						/>
						{a.forward_smile && b.forward_smile && (
							<SmileFitChart
								title="Forward smile: where they part"
								subtitle={`Options on S(T) / S(${a.forward_smile.start.toFixed(2)}): today's surface does not quote them`}
								series={[
									smileSeries(a.forward_smile, "Local vol", colors[0])[0]!,
									smileSeries(b.forward_smile, otherName, colors[1])[0]!,
								]}
							/>
						)}
					</div>
					<div className="grid grid-cols-1 gap-3 lg:grid-cols-[2fr_1fr]">
						<TerminalLaws
							a={a}
							b={b}
							otherName={otherName}
							colors={colors}
							marketColor={t.inkMuted}
						/>
						<BarrierTable a={a} b={b} otherName={otherName} />
					</div>
				</>
			)}
		</div>
	);
}

const last = (d: DistributionResponse) => d.smiles[d.smiles.length - 1]!;

function TerminalLaws({
	a,
	b,
	otherName,
	colors,
	marketColor,
}: {
	a: DistributionResponse;
	b: DistributionResponse;
	otherName: string;
	colors: string[];
	marketColor: string;
}) {
	const n = a.time_grid.length - 1;
	const h = (d: DistributionResponse, label: string, color: string) => ({
		label,
		lo: d.histogram.lo[n]!,
		hi: d.histogram.hi[n]!,
		counts: d.histogram.counts[n]!,
		total: d.n_paths,
		color,
		outline: true,
	});
	const market = a.histogram.market_density?.[n];
	const x = binCentres(
		a.histogram.lo[n]!,
		a.histogram.hi[n]!,
		a.histogram.counts[n]!.length,
	);
	return (
		<DensityChart
			title={`Law of S at ${a.time_grid[n]!.toFixed(2)}y`}
			histograms={[h(a, "Local vol", colors[0]!), h(b, otherName, colors[1]!)]}
			curves={
				market
					? [
							{
								label: "Market (stored surface)",
								x,
								y: market,
								dashed: true,
								color: marketColor,
							},
						]
					: []
			}
			xLabel="S"
		/>
	);
}

function BarrierTable({
	a,
	b,
	otherName,
}: {
	a: DistributionResponse;
	b: DistributionResponse;
	otherName: string;
}) {
	const rows = [
		["Local vol", a],
		[otherName, b],
	] as const;
	const bar = a.barrier;
	return (
		<div className="flex flex-col gap-2 rounded-md border border-hairline bg-surface p-4 text-sm">
			<h3 className="font-semibold text-ink">Up-and-out call</h3>
			<p className="text-xs text-ink-muted">
				{bar
					? `Strike ${bar.strike.toFixed(1)}, barrier ${bar.barrier.toFixed(1)} on the grid dates; 95% intervals.`
					: "—"}
			</p>
			<table className="w-full font-mono text-xs">
				<thead className="text-ink-muted">
					<tr>
						<th className="text-left font-normal">Model</th>
						<th className="pl-4 text-right font-normal">Barrier</th>
						<th className="pl-4 text-right font-normal">Vanilla</th>
					</tr>
				</thead>
				<tbody>
					{rows.map(([name, d]) => (
						<tr key={name}>
							<td className="py-1 text-ink-secondary">{name}</td>
							<td className="pl-4 text-right whitespace-nowrap text-ink">
								{d.barrier
									? `${d.barrier.price.toFixed(3)} ± ${(1.96 * d.barrier.stderr).toFixed(3)}`
									: "—"}
							</td>
							<td className="pl-4 text-right whitespace-nowrap text-ink">
								{d.barrier
									? `${d.barrier.vanilla_price.toFixed(3)} ± ${(1.96 * d.barrier.vanilla_stderr).toFixed(3)}`
									: "—"}
							</td>
						</tr>
					))}
				</tbody>
			</table>
			<p className="text-xs text-ink-muted">
				ATM vol at T: local vol {pct(a.atm_vol[a.atm_vol.length - 1], 2)},{" "}
				{otherName.toLowerCase()} {pct(b.atm_vol[b.atm_vol.length - 1], 2)}.
			</p>
		</div>
	);
}
