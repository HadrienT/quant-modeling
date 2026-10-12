import type { XvaForeignMarket } from "@/shared/api";
import { bp, pct, tenorLabel } from "./format";

const signedCorrelation = (v: number) =>
	(v < 0 ? "−" : "+") + Math.abs(v).toFixed(2);

/**
 * What the second currency rests on: its rate model, the exchange rate and
 * its volatility read on traded straddles, and the three correlations —
 * historical estimates, shown with their confidence interval because no
 * price implies them.
 */
export function ForeignMarketPanel({
	market,
	domestic,
}: {
	market: XvaForeignMarket;
	domestic: string;
}) {
	const pair = `${market.currency}/${domestic}`;
	const correlations = [
		{
			label: `${domestic} rates with ${market.currency} rates`,
			estimate: market.correlation_rates,
		},
		{
			label: `${domestic} rates with ${pair}`,
			estimate: market.correlation_domestic_fx,
		},
		{
			label: `${market.currency} rates with ${pair}`,
			estimate: market.correlation_foreign_fx,
		},
	];
	return (
		<section className="flex flex-col gap-3 rounded-sm border border-hairline bg-surface p-3">
			<div className="flex flex-col gap-1">
				<h2 className="text-sm font-semibold text-ink">
					The second currency: {market.currency}
				</h2>
				<p className="text-xs leading-relaxed text-ink-secondary">
					{pair} {market.spot.toFixed(4)} (ECB reference rate of{" "}
					{market.spot_as_of}). {market.currency} rates: Hull-White with mean
					reversion {pct(market.hull_white.mean_reversion, 1)} and volatility{" "}
					{bp(market.hull_white.sigma)}, calibrated to{" "}
					{bp(market.hull_white.rmse_bp * 1e-4, 1)} on{" "}
					{market.hull_white.swaption_trades} swaption trades. Everything is
					valued in {domestic}.
				</p>
			</div>
			<div className="grid gap-4 lg:grid-cols-2">
				<div className="flex flex-col gap-1">
					<table className="w-full text-sm">
						<caption className="pb-1 text-left text-xs font-medium text-ink-secondary">
							Volatility of {pair}, at the money
						</caption>
						<thead className="text-left text-xs text-ink-muted">
							<tr>
								<th className="py-1 font-normal">Expiry</th>
								<th className="py-1 text-right font-normal">Traded</th>
								<th className="py-1 text-right font-normal">Quartiles</th>
								<th className="py-1 text-right font-normal">Straddles</th>
								<th className="py-1 text-right font-normal">Model</th>
							</tr>
						</thead>
						<tbody>
							{market.fx_volatilities.map((v) => (
								<tr key={v.expiry} className="border-t border-hairline">
									<th
										scope="row"
										className="py-1 text-left font-normal text-ink-secondary"
									>
										{tenorLabel(v.expiry)}
										{v.expiry === market.calibration_expiry && (
											<span className="ml-1 text-2xs text-accent">
												calibrated
											</span>
										)}
									</th>
									<td className="py-1 text-right tabular-nums">
										{pct(v.market, 1)}
									</td>
									<td className="py-1 text-right text-xs text-ink-muted tabular-nums">
										{pct(v.low, 1)} – {pct(v.high, 1)}
									</td>
									<td className="py-1 text-right text-xs text-ink-muted tabular-nums">
										{v.straddles}
									</td>
									<td className="py-1 text-right tabular-nums">
										{pct(v.model, 1)}
									</td>
								</tr>
							))}
						</tbody>
					</table>
					<p className="text-2xs leading-relaxed text-ink-muted">
						Medians of the straddles traded since {market.fx_window_start}{" "}
						(DTCC), not dealer quotes. The model has one volatility for the
						spot, {pct(market.spot_volatility, 1)}: it matches the market at the
						calibrated expiry and has its own term structure elsewhere.
					</p>
				</div>
				<div className="flex flex-col gap-1">
					<table className="w-full text-sm">
						<caption className="pb-1 text-left text-xs font-medium text-ink-secondary">
							Correlations, estimated on history
						</caption>
						<thead className="text-left text-xs text-ink-muted">
							<tr>
								<th className="py-1 font-normal">Between</th>
								<th className="py-1 text-right font-normal">Estimate</th>
								<th className="py-1 text-right font-normal">95 % interval</th>
							</tr>
						</thead>
						<tbody>
							{correlations.map(({ label, estimate }) => (
								<tr key={label} className="border-t border-hairline">
									<th
										scope="row"
										className="py-1 text-left font-normal text-ink-secondary"
									>
										{label}
									</th>
									<td className="py-1 text-right tabular-nums">
										{signedCorrelation(estimate.value)}
									</td>
									<td className="py-1 text-right text-xs text-ink-muted tabular-nums">
										{signedCorrelation(estimate.low)} to{" "}
										{signedCorrelation(estimate.high)}
									</td>
								</tr>
							))}
						</tbody>
					</table>
					<p className="text-2xs leading-relaxed text-ink-muted">
						{market.correlation_rates.weeks} weekly changes from{" "}
						{market.correlation_start} to {market.correlation_end} of{" "}
						{market.correlation_series.join(", ")}. No freely published price
						implies a correlation: these are estimates, and an interval that
						contains zero says the data do not tell the sign.
					</p>
				</div>
			</div>
		</section>
	);
}
