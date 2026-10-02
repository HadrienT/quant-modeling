import { Link } from "@tanstack/react-router";
import { ArrowUpRight } from "lucide-react";
import { useRatesCurves, useRatesQuoteSet } from "@/shared/api";
import { rateBp, ratePct, tenorLabel } from "@/shared/format";
import { Badge, Methodology } from "@/shared/ui";
import { ChartSkeleton, ErrorState } from "@/shared/ui/states";
import { RatesCurveChart } from "@/shared/viz";

/**
 * The USD swap market as it traded: the SOFR swap curve (per tenor, the
 * median fixed rate of the swaps traded on the latest day) with the zero and
 * forward curves bootstrapped from it, and the ATM swaption normal vols
 * implied by traded premiums — each with the number of trades behind it and
 * what was left out (api/app/swaption_market.py). No fallback: a stale or
 * missing store is an error that says what is missing.
 */
export function SwapMarketPanel() {
	const set = useRatesQuoteSet("usd-sofr");
	const built = useRatesCurves(set.data?.curves);
	const trades = set.data?.trades;
	const period = tenorLabel(1 / (set.data?.curves.float_frequency ?? 1));
	return (
		<section className="flex flex-col gap-3 rounded-sm border border-hairline bg-surface p-4">
			<div className="flex flex-wrap items-center justify-between gap-2">
				<h2 className="text-sm font-semibold text-ink">
					SOFR swaps and swaptions, as traded
				</h2>
				<Link
					to="/price"
					search={{ product: "swap" }}
					className="inline-flex items-center gap-1 text-xs text-accent hover:underline"
				>
					Price a swap or a swaption on this curve
					<ArrowUpRight className="size-3.5" />
				</Link>
			</div>
			{set.error ? (
				<ErrorState error={set.error} onRetry={() => set.refetch()} />
			) : !set.data || !trades ? (
				<ChartSkeleton />
			) : (
				<>
					<p className="flex flex-wrap items-center gap-2 text-xs text-ink-secondary">
						<Badge tone="good">Traded prices</Badge>
						{set.data.label}
					</p>
					<RatesCurveChart
						title="SOFR swap curve"
						height={280}
						series={[
							{
								label: "Par swap rate (median of trades)",
								points: trades.swap_rates.map((r) => ({
									x: r.tenor,
									y: r.rate,
								})),
								// Quotes: dots, not joined by an interpolation.
								markers: true,
								line: false,
							},
							...(built.data
								? [
										{
											label: "Zero rate (continuous)",
											points: built.data.curves.points.map((p) => ({
												x: p.tenor,
												y: p.ois_zero,
											})),
										},
										{
											label: `${period} forward`,
											points: built.data.curves.points.map((p) => ({
												x: p.tenor,
												y: p.ois_forward,
											})),
										},
									]
								: []),
						]}
					/>
					<div className="grid gap-4 lg:grid-cols-2">
						<div className="overflow-x-auto">
							<table className="w-full text-sm">
								<caption className="pb-1 text-left text-xs text-ink-muted">
									Swap curve of {set.data.as_of}, from traded swaps
								</caption>
								<thead>
									<tr className="text-left text-xs text-ink-muted">
										<th className="px-2 py-1 font-normal">Tenor</th>
										<th className="px-2 py-1 text-right font-normal">
											Median rate
										</th>
										<th className="px-2 py-1 text-right font-normal">Trades</th>
									</tr>
								</thead>
								<tbody className="font-mono tabular-nums">
									{trades.swap_rates.map((r) => (
										<tr key={r.tenor} className="border-t border-hairline">
											<td className="px-2 py-1 font-sans">
												{tenorLabel(r.tenor)}
											</td>
											<td className="px-2 py-1 text-right">
												{ratePct(r.rate)}
											</td>
											<td className="px-2 py-1 text-right">{r.trades}</td>
										</tr>
									))}
								</tbody>
							</table>
						</div>
						<div className="overflow-x-auto">
							<table className="w-full text-sm">
								<caption className="pb-1 text-left text-xs text-ink-muted">
									ATM swaption normal vols, from premiums traded since{" "}
									{trades.window_start}
								</caption>
								<thead>
									<tr className="text-left text-xs text-ink-muted">
										<th className="px-2 py-1 font-normal">Swaption</th>
										<th className="px-2 py-1 text-right font-normal">
											Median vol
										</th>
										<th className="px-2 py-1 text-right font-normal">
											Quartiles
										</th>
										<th className="px-2 py-1 text-right font-normal">Trades</th>
									</tr>
								</thead>
								<tbody className="font-mono tabular-nums">
									{trades.swaption_vols.map((v) => (
										<tr
											key={`${v.expiry}-${v.tenor}`}
											className="border-t border-hairline"
										>
											<td className="px-2 py-1 font-sans">
												{tenorLabel(v.expiry)} into {tenorLabel(v.tenor)}
											</td>
											<td className="px-2 py-1 text-right">
												{rateBp(v.normal_vol)}
											</td>
											<td className="px-2 py-1 text-right text-ink-secondary">
												{rateBp(v.low)} to {rateBp(v.high)}
											</td>
											<td className="px-2 py-1 text-right">{v.trades}</td>
										</tr>
									))}
								</tbody>
							</table>
						</div>
					</div>
					<p className="text-xs text-ink-secondary">
						{trades.trades_used} swaption trades used.
					</p>
					{trades.rejected.length > 0 && (
						<ul className="text-xs text-ink-secondary">
							{trades.rejected.map((r) => (
								<li key={r.reason}>
									<span className="font-mono tabular-nums">{r.trades}</span>{" "}
									swaption trades left out: {r.reason}
								</li>
							))}
						</ul>
					)}
					<Methodology
						id="swap-market-methodology"
						title="How the swap curve and the vols are built"
						sections={set.data.methodology}
					/>
				</>
			)}
		</section>
	);
}
