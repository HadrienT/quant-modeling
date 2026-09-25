import { Field } from "@/shared/ui";
import { tickerList, type Underlyings } from "./underlyings";

/** The underlyings of a script reading spot(0) to spot(n-1): tickers when
 * the model reads the market (spot, dividend, implied vol and historical
 * correlation come from the database), typed inputs otherwise. */
export function UnderlyingsFields(props: {
	n: number;
	market: boolean;
	u: Underlyings;
}) {
	const { n, u } = props;
	if (props.market) {
		const list = tickerList(u.tickers);
		return (
			<div className="flex flex-col gap-1">
				<Field
					label={`Tickers — one per underlying, spot(0) to spot(${n - 1})`}
					value={u.tickers}
					onChange={(e) => u.setTickers(e.target.value)}
				/>
				<p className="text-2xs text-ink-muted">
					{list.length < n
						? `The script reads ${n} underlyings: ${n - list.length} more ticker(s) needed.`
						: list
								.slice(0, n)
								.map((t, i) => `spot(${i}) = ${t}`)
								.join(" · ")}
				</p>
			</div>
		);
	}
	return (
		<div className="flex flex-col gap-2">
			<span className="text-2xs text-ink-muted uppercase">
				Underlyings (typed, correlated Black-Scholes)
			</span>
			{u.assets.slice(0, n).map((a, i) => (
				<div
					key={i}
					className="grid grid-cols-[4rem_1fr_1fr_1fr] items-end gap-2"
				>
					<span className="pb-1 font-mono text-2xs text-ink-muted">
						spot({i})
					</span>
					<Field
						label="Spot"
						inputMode="decimal"
						value={a.spot}
						onChange={(e) => u.setAsset(i, { spot: e.target.value })}
					/>
					<Field
						label="Vol %"
						inputMode="decimal"
						value={a.volPct}
						onChange={(e) => u.setAsset(i, { volPct: e.target.value })}
					/>
					<Field
						label="Dividend %"
						inputMode="decimal"
						value={a.divPct}
						onChange={(e) => u.setAsset(i, { divPct: e.target.value })}
					/>
				</div>
			))}
			<Field
				label="Correlation % (every pair)"
				inputMode="decimal"
				value={u.corrPct}
				onChange={(e) => u.setCorrPct(e.target.value)}
			/>
		</div>
	);
}
