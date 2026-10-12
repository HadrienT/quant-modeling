import type { RateSensitivities } from "@/shared/portfolio";
import { NumberCell } from "@/shared/ui";

const COLUMNS: {
	key: "pv01Usd" | "pv01Eur" | "fxDelta" | "normalVega";
	label: string;
	title: string;
}[] = [
	{
		key: "pv01Usd",
		label: "PV01 USD",
		title:
			"Change of the value for one basis point more on every zero rate of the SOFR swap curve",
	},
	{
		key: "pv01Eur",
		label: "PV01 EUR",
		title:
			"Change of the value for one basis point more on every zero rate of the Euribor swap curve",
	},
	{
		key: "fxDelta",
		label: "FX delta (EUR)",
		title:
			"The euros the trade is long of: the change of its value per dollar of move of EUR/USD",
	},
	{
		key: "normalVega",
		label: "Vega (1 bp)",
		title: "Change of the value for one basis point more of normal volatility",
	},
];

/**
 * The risks of the rate and FX trades, one row per trade, for the position
 * held and in the currency of its mark: they are bumps of the market the
 * trade is marked on (api/app/portfolio_rates.py). A dash where a trade does
 * not depend on the quantity.
 */
export function RateRiskTable({ rows }: { rows: RateSensitivities[] }) {
	if (rows.length === 0) return null;
	return (
		<section
			aria-label="Rate and FX sensitivities"
			className="flex flex-col gap-1.5"
		>
			<div className="overflow-x-auto rounded-md border border-hairline bg-surface">
				<table className="w-full text-xs">
					<thead>
						<tr className="text-2xs text-ink-muted uppercase">
							<th className="p-2 text-left">Rate / FX trade</th>
							<th className="p-2 text-left">Currency</th>
							{COLUMNS.map((c) => (
								<th key={c.key} className="p-2 text-right" title={c.title}>
									{c.label}
								</th>
							))}
						</tr>
					</thead>
					<tbody>
						{rows.map((r) => (
							<tr key={r.instrumentId} className="border-t border-hairline">
								<td className="p-2 text-left text-ink">{r.label}</td>
								<td className="p-2 text-left text-ink-secondary">
									{r.currency}
								</td>
								{COLUMNS.map((c) => (
									<td key={c.key} className="p-2 text-right">
										{r[c.key] == null ? (
											<span className="text-ink-muted">—</span>
										) : (
											<NumberCell
												value={r[c.key]!}
												signed
												className="text-xs"
											/>
										)}
									</td>
								))}
							</tr>
						))}
					</tbody>
				</table>
			</div>
			<p className="text-2xs text-ink-muted">
				For the position held, in the currency shown. PV01: one basis point more
				on every zero rate of the swap curve. FX delta: the euros the trade is
				long of. Vega: one basis point more of normal volatility.
			</p>
		</section>
	);
}
