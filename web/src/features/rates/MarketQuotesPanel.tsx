import type { RatesMarketResponse } from "@/shared/api";
import { bp, pct, tenorLabel } from "./format";

/**
 * What the market quotes rest on: per swap tenor and per swaption, the
 * number of trades behind the median, the spread between them, and the
 * trades that were left out with the reason. The quotes themselves are in
 * the editable tables below (api/app/swaption_market.py).
 */
export function MarketQuotesPanel({ market }: { market: RatesMarketResponse }) {
	const rejected = market.rejected.reduce((n, r) => n + r.trades, 0);
	return (
		<section className="flex flex-col gap-3 rounded-sm border border-hairline bg-surface p-3">
			<h2 className="text-sm font-semibold text-ink">
				Trades behind the quotes
			</h2>
			<p className="text-xs text-ink-secondary">
				Swap curve of {market.as_of}: per tenor, the median fixed rate of the
				swaps traded that day. Swaption vols: per point, the median normal vol
				implied by the premiums of the swaptions traded since{" "}
				{market.window_start}, with the quartiles. {market.trades_used} swaption
				trades used, {rejected} left out.
			</p>
			<div className="grid gap-4 lg:grid-cols-2">
				<div className="overflow-x-auto">
					<table className="w-full text-sm">
						<caption className="pb-1 text-left text-xs text-ink-muted">
							Swaption vols from traded premiums
						</caption>
						<thead>
							<tr className="text-left text-xs text-ink-muted">
								<th className="px-2 py-1 font-normal">Swaption</th>
								<th className="px-2 py-1 text-right font-normal">Median vol</th>
								<th className="px-2 py-1 text-right font-normal">Quartiles</th>
								<th className="px-2 py-1 text-right font-normal">Trades</th>
							</tr>
						</thead>
						<tbody className="font-mono tabular-nums">
							{market.swaption_vols.map((v) => (
								<tr
									key={`${v.expiry}-${v.tenor}`}
									className="border-t border-hairline"
								>
									<td className="px-2 py-1 font-sans">
										{tenorLabel(v.expiry)} into {tenorLabel(v.tenor)}
									</td>
									<td className="px-2 py-1 text-right">{bp(v.normal_vol)}</td>
									<td className="px-2 py-1 text-right text-ink-secondary">
										{bp(v.low)} to {bp(v.high)}
									</td>
									<td className="px-2 py-1 text-right">{v.trades}</td>
								</tr>
							))}
						</tbody>
					</table>
				</div>
				<div className="overflow-x-auto">
					<table className="w-full text-sm">
						<caption className="pb-1 text-left text-xs text-ink-muted">
							SOFR swap curve from traded swaps
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
							{market.swap_rates.map((r) => (
								<tr key={r.tenor} className="border-t border-hairline">
									<td className="px-2 py-1 font-sans">{tenorLabel(r.tenor)}</td>
									<td className="px-2 py-1 text-right">{pct(r.rate)}</td>
									<td className="px-2 py-1 text-right">{r.trades}</td>
								</tr>
							))}
						</tbody>
					</table>
				</div>
			</div>
			{market.rejected.length > 0 && (
				<ul className="text-xs text-ink-secondary">
					{market.rejected.map((r) => (
						<li key={r.reason}>
							<span className="font-mono tabular-nums">{r.trades}</span>{" "}
							swaption trades left out: {r.reason}
						</li>
					))}
				</ul>
			)}
		</section>
	);
}
