import { useState } from "react";
import { type MarketId, useMarkets, useTickers } from "@/shared/api";
import { Combobox, Label } from "@/shared/ui";

/**
 * Market, then ticker — the listed names the database holds (the same
 * lists as the Market data page). Returns the ticker and its name.
 */
export function TickerPicker({
	label,
	ticker,
	onTicker,
}: {
	label: string;
	ticker: string;
	onTicker: (ticker: string, name?: string) => void;
}) {
	const [market, setMarket] = useState<MarketId>("SP500");
	const markets = useMarkets();
	const tickers = useTickers(market);
	const members = tickers.data?.members ?? [];

	return (
		<div className="flex flex-col gap-1 sm:col-span-2">
			<Label>{label}</Label>
			<div className="flex flex-wrap items-center gap-1.5">
				<select
					aria-label="Market"
					className="h-9 rounded-sm border border-hairline bg-surface px-2 text-sm text-ink"
					value={market}
					onChange={(e) => {
						setMarket(e.target.value as MarketId);
						onTicker("");
					}}
				>
					{(markets.data?.markets ?? []).map((m) => (
						<option key={m.id} value={m.id}>
							{m.name} · {m.currency}
						</option>
					))}
				</select>
				<div className="min-w-56 flex-1">
					<Combobox
						options={members.map((m) => ({
							value: m.ticker,
							label: m.name ? `${m.ticker} — ${m.name}` : m.ticker,
						}))}
						value={ticker || null}
						onChange={(t) =>
							onTicker(
								t,
								members.find((m) => m.ticker === t)?.name ?? undefined,
							)
						}
						placeholder="Select a ticker"
					/>
				</div>
			</div>
		</div>
	);
}
