import { useMemo } from "react";
import { formatNumber } from "@/shared/format";
import { Button } from "@/shared/ui";
import { SurfaceView } from "@/shared/viz";
import { type MarketVega, useMarketVega, vegaSurface } from "./marketVega";

const pm = (v: number, se: number) =>
	`${formatNumber(v, "greek")} ± ${formatNumber(se, "greek")}`;

/**
 * Vega by listed option (lot 17h): the product's sensitivity to each quoted
 * implied vol of the stored chain, through the local-vol AAD, the Dupire
 * formula and each SVI fit. On request: a few seconds of computation.
 */
export function MarketVegaPanel({ body }: { body: unknown }) {
	const q = useMarketVega();
	return (
		<div className="flex flex-col gap-3">
			<Button
				size="sm"
				variant="secondary"
				className="w-fit"
				disabled={q.isPending}
				onClick={() => q.mutate(body)}
			>
				{q.isPending
					? "Differentiating through the calibration…"
					: "Vega by listed option (AAD)"}
			</Button>
			{q.error && <p className="text-sm text-critical">{q.error.message}</p>}
			{q.data && <Result v={q.data} />}
		</div>
	);
}

function Result({ v }: { v: MarketVega }) {
	const grid = useMemo(() => vegaSurface(v), [v]);
	const top = [...v.quotes]
		.sort((a, b) => Math.abs(b.vega) - Math.abs(a.vega))
		.slice(0, 12);
	return (
		<div className="flex flex-col gap-3 rounded-md border border-hairline bg-surface p-4">
			<div className="flex flex-wrap items-baseline gap-x-6 gap-y-1">
				<span className="text-2xs text-ink-muted uppercase">
					{v.ticker} chain of {v.snapshot}
				</span>
				<span className="text-sm text-ink">
					Parallel vega{" "}
					<span className="font-mono">
						{pm(v.total_vega, v.total_std_error)}
					</span>{" "}
					per vol point, over {v.quotes.length} quotes
				</span>
			</div>
			<SurfaceView
				grid={grid}
				mode="divergent"
				title="Vega by maturity and moneyness"
				height={360}
			/>
			<div className="grid gap-4 lg:grid-cols-2">
				<table className="text-2xs">
					<thead className="text-ink-muted uppercase">
						<tr>
							<th className="text-left">Maturity</th>
							<th className="text-right">Vega / vol pt</th>
							<th className="text-right">Quotes</th>
						</tr>
					</thead>
					<tbody className="font-mono tabular-nums">
						{v.maturities.map((m) => (
							<tr key={m.ttm}>
								<td>{m.ttm.toFixed(3)}y</td>
								<td className="text-right">{pm(m.vega, m.std_error)}</td>
								<td className="text-right">{m.n_quotes}</td>
							</tr>
						))}
					</tbody>
				</table>
				<table className="text-2xs">
					<thead className="text-ink-muted uppercase">
						<tr>
							<th className="text-left">Largest quotes</th>
							<th className="text-right">Implied vol</th>
							<th className="text-right">Vega / vol pt</th>
						</tr>
					</thead>
					<tbody className="font-mono tabular-nums">
						{top.map((r, i) => (
							<tr key={i}>
								<td>
									{r.ttm.toFixed(2)}y · K {formatNumber(r.strike, "plain")}
								</td>
								<td className="text-right">
									{formatNumber(r.implied_vol, "vol")}
								</td>
								<td className="text-right">{pm(r.vega, r.std_error)}</td>
							</tr>
						))}
					</tbody>
				</table>
			</div>
			<p className="text-2xs text-ink-muted">
				{v.method} Each number is the price change for +1 vol point on that one
				quote. Moving a quote refits its whole SVI slice, so neighbouring quotes
				can carry opposite signs; the maturity totals and the parallel vega are
				the robust figures. Short maturities may show offsetting values the
				discretised Dupire grid does not cancel exactly (issue #82).
			</p>
		</div>
	);
}
