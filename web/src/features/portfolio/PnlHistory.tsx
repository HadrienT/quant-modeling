import { useMemo, useState } from "react";
import type { Portfolio, PnlWindow } from "@/shared/api";
import { usePortfolioHistory } from "@/shared/api";
import { PnlHistoryChart } from "@/shared/viz";

const WINDOWS: PnlWindow[] = ["1M", "3M", "6M", "1Y", "ALL"];

/**
 * Daily P&L through time. Each past day is valued with that day's market
 * (closes, curves, FX), and the day's P&L includes the day's trades. The
 * cumulative P&L is since the first trade, so every window ends on today's
 * total P&L.
 */
export function PnlHistory({ pf }: { pf: Portfolio }) {
	const [window, setWindow] = useState<PnlWindow>("3M");
	const history = usePortfolioHistory(pf, window);
	const points = useMemo(
		() =>
			history.data?.points.map((p) => ({
				time: p.date,
				daily: p.daily_pnl,
				cumulative: p.cumulative_pnl,
			})),
		[history.data],
	);

	return (
		<div className="flex flex-col gap-2">
			<div role="group" aria-label="Window" className="flex gap-1 self-end">
				{WINDOWS.map((w) => (
					<button
						key={w}
						type="button"
						aria-pressed={w === window}
						onClick={() => setWindow(w)}
						className={
							"rounded-sm border px-2 py-0.5 text-xs " +
							(w === window
								? "border-accent text-ink"
								: "border-hairline text-ink-secondary")
						}
					>
						{w}
					</button>
				))}
			</div>
			<PnlHistoryChart
				points={points}
				currency={pf.base_currency}
				isLoading={history.isLoading}
				error={history.error}
			/>
			{history.data?.warnings.map((w) => (
				<p key={w} className="text-xs text-warning">
					{w}
				</p>
			))}
			<p className="text-2xs text-ink-muted">
				Cumulative P&L is since the first trade (inception to date): whatever
				the window, it ends on the total P&L above.
			</p>
		</div>
	);
}
