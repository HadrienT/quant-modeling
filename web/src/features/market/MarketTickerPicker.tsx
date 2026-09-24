import type { MarketId, MarketInfo, TickerInfo } from "@/shared/api";
import { Combobox } from "@/shared/ui";

const label = (m: TickerInfo) =>
	m.kind === "index"
		? `${m.ticker} — ${m.name ?? "index"} (index)`
		: m.name
			? `${m.ticker} — ${m.name}`
			: m.ticker;

/**
 * Market first, then ticker. The markets come from the API (the ones the
 * database actually holds), each with its members, the index first; a
 * ticker is shown with its company name where the source list has one.
 */
export function MarketTickerPicker({
	markets,
	market,
	members,
	ticker,
	onMarket,
	onTicker,
}: {
	markets: MarketInfo[];
	market: MarketId;
	members: TickerInfo[];
	ticker: string;
	onMarket: (m: MarketId) => void;
	onTicker: (t: string) => void;
}) {
	return (
		<div className="flex flex-wrap items-center gap-2">
			<div role="group" aria-label="Market" className="flex gap-1">
				{markets.map((m) => (
					<button
						key={m.id}
						type="button"
						aria-pressed={m.id === market}
						title={`${m.members} members · ${m.currency}`}
						onClick={() => onMarket(m.id)}
						className={
							"rounded-sm border px-2 py-1 text-xs whitespace-nowrap " +
							(m.id === market
								? "border-accent text-ink"
								: "border-hairline text-ink-secondary")
						}
					>
						{m.name}
					</button>
				))}
			</div>
			<div className="w-72">
				<Combobox
					options={members.map((m) => ({ value: m.ticker, label: label(m) }))}
					value={ticker || null}
					onChange={onTicker}
					placeholder="Select a ticker"
				/>
			</div>
		</div>
	);
}
