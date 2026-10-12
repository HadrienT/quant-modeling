import type { RiskTotals as Totals } from "@/shared/portfolio";
import { DeltaBadge, Metric, MetricRow, NumberCell } from "@/shared/ui";

/**
 * The greeks of the whole portfolio, in its base currency. A delta in shares
 * of one underlying does not add to a delta in shares of another: what is
 * added is money, for the same move of every underlying — said under each
 * figure. The table below keeps the greeks of each underlying as they are.
 */
export function RiskTotals({ totals, base }: { totals: Totals; base: string }) {
	return (
		<section aria-label="Portfolio greeks" className="flex flex-col gap-1.5">
			<MetricRow className="xl:grid-cols-5">
				<Metric
					label="Delta"
					unit={base}
					value={<NumberCell value={totals.delta} className="text-xl" />}
					footnote="equity exposure: +1 % on every underlying gains 1 % of it"
				/>
				<Metric
					label="Gamma"
					unit={base}
					value={<DeltaBadge value={totals.gamma} className="text-xl" />}
					footnote="change of the delta for +1 % on every underlying"
				/>
				<Metric
					label="Vega"
					unit={base}
					value={<DeltaBadge value={totals.vega} className="text-xl" />}
					footnote="for one volatility point more on every underlying"
				/>
				<Metric
					label="Theta"
					unit={base}
					value={<DeltaBadge value={totals.theta} className="text-xl" />}
					footnote="over one day, the market unchanged"
				/>
				<Metric
					label="Rates"
					unit={base}
					value={<DeltaBadge value={totals.rates} className="text-xl" />}
					footnote="for one basis point more on every curve"
				/>
			</MetricRow>
			<p className="text-2xs text-ink-muted">
				Sums over {totals.counted} valued position
				{totals.counted === 1 ? "" : "s"}, converted at today&apos;s exchange
				rates. They assume every underlying moves together, which a portfolio of
				different names does not do: the table below gives each
				underlying&apos;s own greeks.
				{totals.withoutSpot > 0 &&
					` ${totals.withoutSpot} position(s) without a spot are left out of the delta and the gamma.`}
			</p>
		</section>
	);
}
