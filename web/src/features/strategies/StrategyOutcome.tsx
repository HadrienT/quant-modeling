import type { Leg, MarketInputs, PayoffResult } from "@/shared/payoff";
import { Metric, MetricRow, NumberCell } from "@/shared/ui";
import { GreekProfileChart, PayoffChart } from "@/shared/viz";

/** Metrics + payoff + greek profiles for the current strategy (WP 10 §3-4). */
export function StrategyOutcome({
	result,
	mkt,
	legs,
}: {
	result: PayoffResult;
	mkt: MarketInputs;
	legs: Leg[];
}) {
	const probProfit =
		result.atMaturity.filter((v) => v > 0).length / result.atMaturity.length;
	const rr =
		typeof result.maxGain === "number" && typeof result.maxLoss === "number"
			? Math.abs(result.maxGain / result.maxLoss)
			: null;

	return (
		<div className="flex flex-col gap-4">
			<MetricRow>
				<Metric
					label="Net premium"
					value={
						<NumberCell value={result.netPremium} magnitude="price" signed />
					}
					footnote={
						result.netPremium < 0
							? "debit — you pay"
							: result.netPremium > 0
								? "credit — you receive"
								: "even"
					}
				/>
				<Metric
					label="Max gain"
					value={
						result.maxGain === "unbounded" ? (
							"unbounded"
						) : (
							<NumberCell value={result.maxGain} magnitude="price" />
						)
					}
				/>
				<Metric
					label="Max loss"
					value={
						result.maxLoss === "unbounded" ? (
							"unbounded"
						) : (
							<NumberCell value={result.maxLoss} magnitude="price" />
						)
					}
				/>
				<Metric
					label="Breakevens"
					value={
						result.breakevens.length
							? result.breakevens.map((b) => b.toFixed(1)).join(" · ")
							: "—"
					}
				/>
				<Metric
					label="Prob. of profit"
					value={`${(probProfit * 100).toFixed(0)}%`}
					footnote="risk-neutral proxy, not real-world"
				/>
			</MetricRow>
			{rr != null && (
				<p className="text-2xs text-ink-muted">
					Risk / reward ≈ {rr.toFixed(2)}
				</p>
			)}

			<PayoffChart
				data={{
					spot: result.spot,
					atMaturity: result.atMaturity,
					atT: result.atT,
					legs: result.legs,
					breakevens: result.breakevens,
					currentSpot: mkt.spot,
					strikes: legs
						.filter((l) => l.kind !== "underlying")
						.map((l) => l.strike),
				}}
			/>
			<GreekProfileChart currentSpot={mkt.spot} profiles={result.greeks} />
		</div>
	);
}
