import { useNavigate, useSearch } from "@tanstack/react-router";
import { useTickers } from "@/shared/api";
import { Combobox, Tabs, TabsContent, TabsList, TabsTrigger } from "@/shared/ui";
import { PricesTab } from "./PricesTab";
import { RatesTab } from "./RatesTab";
import { VolTab } from "./VolTab";

/**
 * Market data — the showcase page (WP 08). Surfaces are the README
 * screenshot. The ticker is one piece of state shared by all three tabs
 * (kept in the URL, so it's also shareable/bookmarkable) -- the selector
 * lives here, above the tabs, rather than duplicated inside each one, so
 * switching from Prices to Volatility never loses or hides it.
 */
export default function MarketPage() {
	const search = useSearch({ from: "/market" });
	const navigate = useNavigate({ from: "/market" });
	const tab = search.tab ?? "prices";
	const ticker = search.ticker ?? "AAPL";
	const tickers = useTickers();

	const set = (patch: Partial<typeof search>) =>
		navigate({ search: (prev) => ({ ...prev, ...patch }) });

	return (
		<div className="mx-auto flex max-w-6xl flex-col gap-4">
			<div className="flex flex-wrap items-center justify-between gap-3">
				<h1 className="text-lg font-semibold text-ink">Market data</h1>
				<div className="w-56">
					<Combobox
						options={(tickers.data?.tickers ?? []).map((t) => ({ value: t }))}
						value={ticker || null}
						onChange={(t) => set({ ticker: t })}
						placeholder="Select a ticker"
					/>
				</div>
			</div>
			<Tabs value={tab} onValueChange={(v) => set({ tab: v as typeof tab })}>
				<TabsList>
					<TabsTrigger value="prices">Prices</TabsTrigger>
					<TabsTrigger value="vol">Volatility</TabsTrigger>
					<TabsTrigger value="rates">Rates</TabsTrigger>
				</TabsList>
				<TabsContent value="prices">
					<PricesTab ticker={ticker} />
				</TabsContent>
				<TabsContent value="vol">
					<VolTab ticker={ticker} />
				</TabsContent>
				<TabsContent value="rates">
					<RatesTab />
				</TabsContent>
			</Tabs>
		</div>
	);
}
