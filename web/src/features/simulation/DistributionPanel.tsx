import type { DistributionResponse } from "@/shared/api/types";
import { DensityChart, FanChart } from "@/shared/viz";
import {
	binCentres,
	forwardAt,
	lognormalPdf,
	pct,
	quantileBands,
} from "./distribution";

/**
 * The law of the spot (WP 29): quantile bands over time with the first
 * paths drawn in, a date cursor, and at that date the histogram of S_t
 * against Black-Scholes at the model's own ATM vol (same forward, same ATM
 * price: what the model adds is the shape) and the stored surface's density
 * when the model is calibrated on a ticker.
 */
export function DistributionPanel({
	data,
	title,
	revealCount,
	cursor,
	setCursor,
}: {
	data: DistributionResponse;
	title: string;
	revealCount?: number;
	cursor: number;
	setCursor: (i: number) => void;
}) {
	const time = data.time_grid;
	const n = time.length - 1;
	const i = Math.min(Math.max(cursor, 1), n);
	const t = time[i]!;
	const { bands, median } = quantileBands(
		data.quantile_levels,
		data.spot_quantiles,
	);
	const h = data.histogram;
	const lo = h.lo[i]!,
		hi = h.hi[i]!;
	const centres = binCentres(lo, hi, h.counts[i]!.length);
	const F = forwardAt(data, t);
	const atm = data.atm_vol[i];
	const market = h.market_density?.[i];
	const arbitrage = data.market?.arbitrage_dates?.some(
		(d) => Math.abs(d - t) < 1e-9,
	);
	const q = (level: number) =>
		data.spot_quantiles[data.quantile_levels.indexOf(level)]?.[i];

	return (
		<section className="flex flex-col gap-3">
			<FanChart
				title={title}
				subtitle={`${data.n_paths.toLocaleString("en-US")} paths for the bands; click or drag to pick a date`}
				time={time}
				bands={bands}
				paths={data.paths}
				lines={median ? [{ label: "Median", y: median }] : []}
				revealCount={revealCount}
				cursor={i}
				onCursor={setCursor}
			/>
			<label className="flex items-center gap-3 text-sm text-ink-secondary">
				<span className="w-28 shrink-0 font-mono text-xs">
					t = {t.toFixed(3)}y
				</span>
				<input
					type="range"
					className="w-full accent-accent"
					min={1}
					max={n}
					value={i}
					onChange={(e) => setCursor(Number(e.target.value))}
					aria-label="Date of the histogram"
				/>
			</label>
			<div className="grid grid-cols-1 gap-3 lg:grid-cols-[2fr_1fr]">
				<DensityChart
					title={`Law of S at t = ${t.toFixed(2)}y`}
					subtitle={
						arbitrage
							? "No market curve: the stored smile at this date is not a density (call-spread or butterfly arbitrage)"
							: "Histogram between the 0.5% and 99.5% quantiles"
					}
					histograms={[
						{
							label: "Model",
							lo,
							hi,
							counts: h.counts[i]!,
							total: data.n_paths,
						},
					]}
					curves={[
						...(atm != null
							? [
									{
										label: `Black-Scholes at the model's ATM vol (${pct(atm)})`,
										x: centres,
										y: centres.map((x) => lognormalPdf(x, F, atm, t)),
										dashed: true,
									},
								]
							: []),
						...(market
							? [{ label: "Market (stored surface)", x: centres, y: market }]
							: []),
					]}
					markers={[{ label: "Forward", x: F }]}
					xLabel="S"
				/>
				<dl className="grid grid-cols-2 content-start gap-x-4 gap-y-2 rounded-md border border-hairline bg-surface p-4 text-sm">
					<Stat label="Forward F(0, t)" value={F.toFixed(2)} />
					<Stat
						label="ATM implied vol"
						value={
							atm == null
								? "—"
								: `${pct(atm, 2)} ± ${pct(1.96 * (data.atm_vol_stderr[i] ?? 0), 2)}`
						}
					/>
					<Stat label="Median" value={q(0.5)?.toFixed(2) ?? "—"} />
					<Stat
						label="5% / 95%"
						value={`${q(0.05)?.toFixed(1) ?? "—"} / ${q(0.95)?.toFixed(1) ?? "—"}`}
					/>
					<Stat
						label="E[S/N] (MC)"
						value={`${data.martingale.mean[i]!.toFixed(3)} ± ${(1.96 * data.martingale.stderr[i]!).toFixed(3)}`}
					/>
					<Stat
						label="E[S/N] exact"
						value={data.martingale.exact[i]!.toFixed(3)}
					/>
				</dl>
			</div>
		</section>
	);
}

function Stat({ label, value }: { label: string; value: string }) {
	return (
		<div className="flex flex-col">
			<dt className="text-2xs tracking-wide text-ink-muted uppercase">
				{label}
			</dt>
			<dd className="font-mono text-ink">{value}</dd>
		</div>
	);
}
