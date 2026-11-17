import { useState } from "react";
import type { DistributionResponse } from "@/shared/api/types";
import { SmileFitChart } from "@/shared/viz";
import { smileSeries } from "./distribution";

/** One tab per vanilla smile, then the forward smile. */
function tabs(data: DistributionResponse) {
	const spot = data.smiles.map((s) => ({
		key: `spot-${s.time}`,
		label: `${s.time.toFixed(2)}y`,
		smile: s,
		forward: false,
	}));
	const fs = data.forward_smile;
	return fs
		? [
				...spot,
				{
					key: "forward",
					label: `Forward ${fs.start.toFixed(2)}y → ${(fs.start + fs.time).toFixed(2)}y`,
					smile: fs,
					forward: true,
				},
			]
		: spot;
}

/**
 * Does the model reprice the market? (WP 29) Out-of-the-money options priced
 * on the paths, inverted with Black-76, with a 95% band from their standard
 * errors, against the stored SVI surface at the same moneyness. The forward
 * smile -- options on S_T / S_t1 -- is what the model says about tomorrow's
 * smile; today's market cannot quote it.
 */
export function SmilePanel({ data }: { data: DistributionResponse }) {
	const all = tabs(data);
	const [key, setKey] = useState(all[all.length > 1 ? all.length - 2 : 0]?.key);
	const cur = all.find((x) => x.key === key) ?? all[0];
	if (!cur) return null;
	const noMarket =
		!cur.forward && !cur.smile.market_iv && data.market
			? data.model === "sabr"
				? "SABR simulates the forward to the last date: only that date is the market's."
				: "Outside the stored surface's quoted maturities."
			: null;
	return (
		<section className="flex flex-col gap-2">
			<div className="flex flex-wrap gap-1" role="tablist">
				{all.map((x) => (
					<button
						key={x.key}
						type="button"
						role="tab"
						aria-selected={x.key === cur.key}
						className={`rounded px-2.5 py-1 font-mono text-xs ${
							x.key === cur.key
								? "bg-accent text-white"
								: "border border-hairline text-ink-secondary"
						}`}
						onClick={() => setKey(x.key)}
					>
						{x.label}
					</button>
				))}
			</div>
			<SmileFitChart
				title={cur.forward ? "Forward smile" : "Implied volatility smile"}
				subtitle={
					cur.forward
						? `Options on S(${(cur.smile.start + cur.smile.time).toFixed(2)}) / S(${cur.smile.start.toFixed(2)}), strikes as a multiple of the forward ratio`
						: (noMarket ??
							`Maturity ${cur.smile.time.toFixed(2)}y, strikes as K / F(0, t); bands are 95% Monte Carlo intervals`)
				}
				series={smileSeries(cur.smile)}
			/>
		</section>
	);
}
