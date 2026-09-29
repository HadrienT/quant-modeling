import { useNavigate, useSearch } from "@tanstack/react-router";
import { Tabs, TabsContent, TabsList, TabsTrigger } from "@/shared/ui";
import { CompaniesTab } from "./CompaniesTab";
import { SpreadsTab } from "./SpreadsTab";

/**
 * Credit — US corporate credit, the way the market page shows rates: market
 * credit spreads bootstrapped into hazard curves, and per company the 10-K /
 * 10-Q statements and a structural (Merton) credit model. Every figure comes
 * from the project's own database (FRED, SEC EDGAR), with its methodology.
 * Tab, company, frequency and recovery live in the URL, so a view is a link.
 */
export default function CreditPage() {
	const search = useSearch({ from: "/credit" });
	const navigate = useNavigate({ from: "/credit" });
	const tab = search.tab ?? "spreads";
	const set = (patch: Partial<typeof search>) =>
		navigate({ search: (prev) => ({ ...prev, ...patch }) });

	return (
		<div className="mx-auto flex max-w-6xl flex-col gap-4">
			<div className="flex flex-col gap-1">
				<h1 className="text-lg font-semibold text-ink">Credit</h1>
				<p className="text-sm text-ink-secondary">
					US corporate credit spreads and hazard curves, company statements from
					SEC filings, and a structural credit model per company.
				</p>
			</div>
			<Tabs value={tab} onValueChange={(v) => set({ tab: v as typeof tab })}>
				<TabsList>
					<TabsTrigger value="spreads">Credit curves</TabsTrigger>
					<TabsTrigger value="companies">Companies</TabsTrigger>
				</TabsList>
				<TabsContent value="spreads">
					<SpreadsTab
						recovery={search.recovery ?? 0.4}
						onRecovery={(recovery) => set({ recovery })}
					/>
				</TabsContent>
				<TabsContent value="companies">
					<CompaniesTab
						ticker={search.ticker ?? "AAPL"}
						frequency={search.freq ?? "annual"}
						onTicker={(ticker) => set({ ticker })}
						onFrequency={(freq) => set({ freq })}
					/>
				</TabsContent>
			</Tabs>
		</div>
	);
}
