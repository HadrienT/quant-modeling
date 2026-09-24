import { useNavigate, useSearch } from "@tanstack/react-router";
import { type MarketId, useMarkets, useTickers } from "@/shared/api";
import { Tabs, TabsContent, TabsList, TabsTrigger } from "@/shared/ui";
import { FxTab } from "./FxTab";
import { MarketTickerPicker } from "./MarketTickerPicker";
import { PricesTab } from "./PricesTab";
import { RatesTab } from "./RatesTab";
import { VolTab } from "./VolTab";

/**
 * Market data — the showcase page (WP 08). Surfaces are the README
 * screenshot. The ticker is state shared by Prices and Volatility (kept in
 * the URL, so it's also shareable/bookmarkable) -- the selector lives here,
 * above the tabs, rather than duplicated inside each one, so switching
 * between them never loses or hides it. Rates has no ticker (a curve isn't
 * per-name), so the selector hides there rather than sitting inert.
 */
export default function MarketPage() {
	const search = useSearch({ from: "/market" });
	const navigate = useNavigate({ from: "/market" });
	const tab = search.tab ?? "prices";
	const market: MarketId = search.market ?? "SP500";
	const markets = useMarkets();
	const tickers = useTickers(market);
	const members = tickers.data?.members ?? [];
	// Default: AAPL on the S&P 500 (as before markets existed), the index
	// itself elsewhere — the first member, which the API lists first.
	const ticker =
		search.ticker ?? (market === "SP500" ? "AAPL" : (members[0]?.ticker ?? ""));
	const info = markets.data?.markets.find((m) => m.id === market);

	const set = (patch: Partial<typeof search>) =>
		navigate({ search: (prev) => ({ ...prev, ...patch }) });

	return (
		<div className="mx-auto flex max-w-6xl flex-col gap-4">
			<div className="flex flex-wrap items-center justify-between gap-3">
				<h1 className="text-lg font-semibold text-ink">Market data</h1>
				{tab !== "rates" && tab !== "fx" && (
					<MarketTickerPicker
						markets={markets.data?.markets ?? []}
						market={market}
						members={members}
						ticker={ticker}
						onMarket={(m) => set({ market: m, ticker: undefined })}
						onTicker={(t) => set({ ticker: t })}
					/>
				)}
			</div>
			<Tabs value={tab} onValueChange={(v) => set({ tab: v as typeof tab })}>
				<TabsList>
					<TabsTrigger value="prices">Prices</TabsTrigger>
					<TabsTrigger value="vol">Volatility</TabsTrigger>
					<TabsTrigger value="rates">Rates</TabsTrigger>
					<TabsTrigger value="fx">FX</TabsTrigger>
				</TabsList>
				<TabsContent value="prices">
					<PricesTab
						ticker={ticker}
						isIndex={members.find((m) => m.ticker === ticker)?.kind === "index"}
					/>
				</TabsContent>
				<TabsContent value="vol">
					<VolTab
						ticker={ticker}
						unavailable={info && !info.has_options ? info.note : null}
					/>
				</TabsContent>
				<TabsContent value="rates">
					<RatesTab
						currency={search.ccy ?? "USD"}
						onCurrency={(ccy) => set({ ccy })}
					/>
				</TabsContent>
				<TabsContent value="fx">
					<FxTab
						base={search.base ?? "EUR"}
						quote={search.quote ?? "USD"}
						onPair={(base, quote) => set({ base, quote })}
					/>
				</TabsContent>
			</Tabs>
		</div>
	);
}
