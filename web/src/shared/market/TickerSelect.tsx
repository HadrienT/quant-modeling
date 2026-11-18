import { useMemo, useState } from "react";
import {
	type MarketId,
	type TickerInfo,
	useMarkets,
	useTickers,
} from "@/shared/api";
import { Combobox } from "@/shared/ui";

/** The exchange rates a script can read as an underlying (`FX:EUR`): the
 * dollar price of one unit of the currency (ECB reference rates). */
const FX = ["EUR", "GBP", "JPY", "CHF"].map((c) => ({
	value: `FX:${c}`,
	label: `FX:${c} · ${c}/USD exchange rate`,
}));

const label = (m: TickerInfo) =>
	m.name ? `${m.ticker} · ${m.name}` : m.ticker;

const box =
	"h-9 shrink-0 rounded-sm border border-hairline bg-surface px-2 text-xs text-ink";

/**
 * A ticker chosen from the database's markets, as the backtest picks its
 * assets: the market first (its members, the index ETFs with the S&P 500),
 * then a searchable, scrollable list by ticker and company name. With
 * `allowFx`, the exchange rates a script can read too. A value that is in
 * no list (a link from before, another market) stays selected.
 */
export function TickerSelect({
	value,
	onChange,
	allowFx = false,
	placeholder = "Select a ticker…",
}: {
	value: string;
	onChange: (ticker: string) => void;
	allowFx?: boolean;
	placeholder?: string;
}) {
	const markets = useMarkets();
	const [market, setMarket] = useState<MarketId>("SP500");
	const tickers = useTickers(market);
	const members = tickers.data?.members;

	const options = useMemo(() => {
		const out = (members ?? []).map((m) => ({
			value: m.ticker,
			label: label(m),
		}));
		if (allowFx) out.push(...FX);
		if (value && !out.some((o) => o.value === value))
			out.unshift({ value, label: value });
		return out;
	}, [members, allowFx, value]);

	return (
		<div className="flex min-w-0 items-center gap-1.5">
			<select
				aria-label="Market"
				className={box}
				value={market}
				onChange={(e) => setMarket(e.target.value as MarketId)}
			>
				{(markets.data?.markets ?? [{ id: "SP500", name: "S&P 500" }]).map(
					(m) => (
						<option key={m.id} value={m.id}>
							{m.name}
						</option>
					),
				)}
			</select>
			<div className="min-w-0 flex-1">
				<Combobox
					options={options}
					value={value || null}
					onChange={onChange}
					placeholder={tickers.isLoading ? "Loading…" : placeholder}
				/>
			</div>
		</div>
	);
}

/**
 * One ticker per underlying of a script, spot(0) to spot(n - 1), kept as
 * the comma-separated list the request and the page's link carry.
 */
export function TickerList({
	value,
	count,
	onChange,
	allowFx = false,
}: {
	value: string;
	count: number;
	onChange: (tickers: string) => void;
	allowFx?: boolean;
}) {
	const list = value.split(/[\s,;]+/).filter(Boolean);
	const slots = Array.from({ length: count }, (_, i) => list[i] ?? "");
	return (
		<div className="flex flex-col gap-1.5">
			{slots.map((t, i) => (
				<div key={i} className="flex items-center gap-2">
					<span className="w-14 shrink-0 font-mono text-2xs text-ink-muted">
						spot({i})
					</span>
					<div className="min-w-0 flex-1">
						<TickerSelect
							value={t}
							allowFx={allowFx}
							onChange={(next) => {
								const out = [...slots];
								out[i] = next;
								onChange(out.join(", "));
							}}
						/>
					</div>
				</div>
			))}
		</div>
	);
}
