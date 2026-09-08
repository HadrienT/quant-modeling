import { useNavigate, useSearch } from "@tanstack/react-router";
import { Tabs, TabsContent, TabsList, TabsTrigger } from "@/shared/ui";
import { PricesTab } from "./PricesTab";
import { RatesTab } from "./RatesTab";
import { VolTab } from "./VolTab";

/** Market data — the showcase page (WP 08). Surfaces are the README screenshot. */
export default function MarketPage() {
	const search = useSearch({ from: "/market" });
	const navigate = useNavigate({ from: "/market" });
	const tab = search.tab ?? "prices";
	const ticker = search.ticker ?? "AAPL";

	const set = (patch: Partial<typeof search>) =>
		navigate({ search: (prev) => ({ ...prev, ...patch }) });

	return (
		<div className="mx-auto flex max-w-6xl flex-col gap-4">
			<h1 className="text-lg font-semibold text-ink">Market data</h1>
			<Tabs value={tab} onValueChange={(v) => set({ tab: v as typeof tab })}>
				<TabsList>
					<TabsTrigger value="prices">Prices</TabsTrigger>
					<TabsTrigger value="vol">Volatility</TabsTrigger>
					<TabsTrigger value="rates">Rates</TabsTrigger>
				</TabsList>
				<TabsContent value="prices">
					<PricesTab ticker={ticker} onTicker={(t) => set({ ticker: t })} />
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
