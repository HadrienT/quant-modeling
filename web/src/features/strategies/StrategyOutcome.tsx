import type { Leg, MarketInputs, PayoffResult } from "@/shared/payoff";
import { Metric, MetricRow, NumberCell } from "@/shared/ui";
import { GreekProfileChart, PayoffChart } from "@/shared/viz";

/** Metrics + payoff + greek profiles for the current strategy (WP 10 §3-4). */
export function StrategyOutcome({
	result,
	mkt,
	legs,
	hoveredLeg,
	onLegHover,
}: {
	result: PayoffResult;
	mkt: MarketInputs;
	legs: Leg[];
	hoveredLeg?: number | null;
	onLegHover?: (index: number | null) => void;
}) {
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
					value={
						result.probProfit == null
							? "—"
							: `${(result.probProfit * 100).toFixed(0)}%`
					}
					footnote="risk-neutral, flat BS vol, to the longest maturity"
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
				hoveredLeg={hoveredLeg}
				onLegHover={onLegHover}
			/>
			<GreekProfileChart currentSpot={mkt.spot} profiles={result.greeks} />
		</div>
	);
}
