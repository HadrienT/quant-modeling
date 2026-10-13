import type { BacktestStrategy } from "@/shared/api";
import { Explained, Figure } from "@/shared/xva";
import { confidence, money, percent } from "./format";
import { SOURCES, SPREAD_ESTIMATORS } from "./texts";

const bps = (fraction: number) => `${(fraction * 1e4).toFixed(0)} bp`;

/**
 * What trading cost the strategy: the value with and without costs side by
 * side, the turnover that produced the difference, and the cost at which the
 * excess return would be gone, with its interval.
 */
export function CostPanel({ data }: { data: BacktestStrategy }) {
	const c = data.cost_summary;
	const last = (run: BacktestStrategy["gross"]) => run?.value?.at(-1);
	const net = last(data.strategy);
	const gross = last(data.gross);
	const even = c.break_even_cost;
	const p = c.participation;

	return (
		<div className="flex flex-col gap-4">
			<div className="grid gap-2 sm:grid-cols-2 lg:grid-cols-4">
				<Figure
					label="Final value, net of costs"
					value={net == null ? "n/a" : money(net)}
					note={data.conventions.costs}
				/>
				<Figure
					label="Final value, before costs"
					value={gross == null ? "n/a" : money(gross)}
					note={
						gross == null
							? "the run was asked without a cost"
							: "the same decisions, traded for free"
					}
				/>
				<Figure
					label="Turnover per year"
					value={
						c.annualized_turnover == null
							? "n/a"
							: percent(c.annualized_turnover, 0)
					}
					note={data.conventions.turnover}
				/>
				<Figure
					label="Cost per year"
					value={
						c.annual_cost_bps == null
							? "n/a"
							: `${c.annual_cost_bps.toFixed(1)} bp`
					}
					note="basis points of the portfolio value, a year"
				/>
			</div>

			<Explained
				title="Break-even cost"
				source={SOURCES.breakEven}
				figures={
					<Figure
						label="Cost that cancels the excess return"
						value={even?.value == null ? "n/a" : bps(even.value)}
						note={
							even?.lower == null || even.upper == null
								? "no trade, no break-even"
								: `${confidence(even)} interval [${bps(even.lower)}, ${bps(even.upper)}]`
						}
					/>
				}
			>
				The proportional cost at which the strategy would earn no more than
				cash. A strategy is worth trading only if its break-even cost is
				comfortably above what a trade really costs; the interval says how well
				the data pin it down.{" "}
				{even && `Convention: ${even.convention}. Interval: ${even.method}.`}
			</Explained>

			{p && (
				<Explained title="Share of the market traded" source={SOURCES.costs}>
					Over {p.executions} executions, an order took at most{" "}
					{p.maximum == null ? "n/a" : percent(p.maximum, 2)} of the amount
					traded that day in its asset (median{" "}
					{p.median == null ? "n/a" : percent(p.median, 4)}), for a book of
					1,000,000. A proportional cost assumes the order does not move the
					price: the assumption weakens as this share grows. {SPREAD_ESTIMATORS}
				</Explained>
			)}
		</div>
	);
}
