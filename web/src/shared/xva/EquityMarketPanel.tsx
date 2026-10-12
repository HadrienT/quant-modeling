import type { XvaEquityMarket } from "@/shared/api";
import { pct } from "./format";

const signedCorrelation = (v: number) =>
	(v < 0 ? "−" : "+") + Math.abs(v).toFixed(2);

/**
 * What the share of a netting set with equity options rests on: its close
 * and dividend yield, the volatility the model is calibrated to and where it
 * comes from, and its correlation with the interest rate — a historical
 * estimate, shown with its confidence interval because no price implies it.
 */
export function EquityMarketPanel({ market }: { market: XvaEquityMarket }) {
	const c = market.correlation;
	return (
		<section className="flex flex-col gap-1 rounded-sm border border-hairline bg-surface p-3">
			<h2 className="text-sm font-semibold text-ink">
				The share: {market.ticker}
			</h2>
			<p className="text-xs leading-relaxed text-ink-secondary">
				{market.ticker} {market.spot.toFixed(2)} (close of {market.spot_as_of}),
				dividend yield {pct(market.dividend_yield)}
				{market.dividend_as_of ? "" : " (none stored)"}. Simulated with the
				interest rate as a lognormal price: volatility{" "}
				{pct(market.spot_volatility, 1)}, so that the option struck at{" "}
				{market.calibration_strike.toLocaleString("en-US")} expiring in{" "}
				{market.calibration_expiry.toFixed(2)} years has its market volatility
				of {pct(market.market_volatility, 1)}
				{market.volatility_proxied ? " (a proxy)" : ""}.
			</p>
			<p className="text-xs leading-relaxed text-ink-muted">
				Volatility: {market.volatility_source}. Correlation with the interest
				rate: {signedCorrelation(c.value)} (95 % interval{" "}
				{signedCorrelation(c.low)} to {signedCorrelation(c.high)}), on {c.weeks}{" "}
				weekly changes of {market.correlation_series.join(" and ")} from{" "}
				{market.correlation_start} to {market.correlation_end}.
			</p>
		</section>
	);
}
