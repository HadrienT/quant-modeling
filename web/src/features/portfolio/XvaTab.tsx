import { useState } from "react";
import type { Portfolio, PortfolioXvaRequest } from "@/shared/api";
import { usePortfolioXva } from "@/shared/api";
import { RATINGS, netQuantities } from "@/shared/portfolio";
import {
	Button,
	Label,
	Methodology,
	Tabs,
	TabsContent,
	TabsList,
	TabsTrigger,
	WarningList,
} from "@/shared/ui";
import { ChartSkeleton, ErrorState } from "@/shared/ui/states";
import {
	AdjustmentsPanel,
	EquityMarketPanel,
	ExposurePanel,
	ForeignMarketPanel,
	MarginCapitalPanel,
	TradesTable,
	XvaResultHeader,
} from "@/shared/xva";

const box =
	"h-9 rounded-sm border border-hairline bg-surface px-2 text-sm text-ink";
const FUNDING_BP = [0, 25, 50, 100];
const PATHS = [2000, 10000, 30000];

/**
 * Exposure and xVA of one netting set of the portfolio: the trades held
 * under it today (rate and FX trades, European equity options), the counterparty's rating and the collateral agreement of
 * the Counterparties tab, on the market of the xVA page. The bank's own side
 * (its rating, its funding spread) is chosen here. A run takes a few
 * seconds and is started by the reader.
 */
export function XvaTab({ pf }: { pf: Portfolio }) {
	const held = netQuantities(pf.transactions ?? []);
	const sets = (pf.counterparties ?? []).flatMap((c) =>
		(c.netting_sets ?? []).map((n) => ({
			id: n.id,
			label: `${c.name} (${c.rating}): ${n.name}`,
			trades: (pf.instruments ?? []).filter(
				(i) => i.netting_set_id === n.id && (held.get(i.id) ?? 0) !== 0,
			).length,
		})),
	);
	const traded = sets.filter((s) => s.trades > 0);
	const [chosen, setChosen] = useState<string | null>(null);
	const [own, setOwn] = useState<PortfolioXvaRequest["own_rating"]>("A");
	const [funding, setFunding] = useState(0);
	const [paths, setPaths] = useState(10000);
	const [request, setRequest] = useState<PortfolioXvaRequest | null>(null);
	const result = usePortfolioXva(request);
	const data = result.data;
	const id = chosen ?? traded[0]?.id ?? null;

	if (traded.length === 0)
		return (
			<p className="max-w-3xl text-sm leading-relaxed text-ink-secondary">
				No open trade is under a netting set yet. Create a counterparty in the
				Counterparties tab, then book a rate or FX trade under one of its
				netting sets — or attach an existing derivative to one.
			</p>
		);

	return (
		<div className="flex flex-col gap-4">
			<div className="flex flex-wrap items-end gap-3 rounded-md border border-hairline bg-surface p-3">
				<label className="flex min-w-60 flex-1 flex-col gap-1">
					<Label>Netting set</Label>
					<select
						className={box}
						value={id ?? ""}
						onChange={(e) => setChosen(e.target.value)}
					>
						{traded.map((s) => (
							<option key={s.id} value={s.id}>
								{s.label} — {s.trades} trade{s.trades > 1 ? "s" : ""}
							</option>
						))}
					</select>
				</label>
				<label className="flex flex-col gap-1">
					<Label>Bank&apos;s own rating</Label>
					<select
						className={box}
						value={own}
						onChange={(e) => setOwn(e.target.value as typeof own)}
					>
						{RATINGS.map((r) => (
							<option key={r}>{r}</option>
						))}
					</select>
				</label>
				<label className="flex flex-col gap-1">
					<Label>Funding spread (bp)</Label>
					<select
						className={box}
						value={funding}
						onChange={(e) => setFunding(Number(e.target.value))}
					>
						{FUNDING_BP.map((b) => (
							<option key={b}>{b}</option>
						))}
					</select>
				</label>
				<label className="flex flex-col gap-1">
					<Label>Paths</Label>
					<select
						className={box}
						value={paths}
						onChange={(e) => setPaths(Number(e.target.value))}
					>
						{PATHS.map((p) => (
							<option key={p}>{p}</option>
						))}
					</select>
				</label>
				<Button
					size="sm"
					disabled={!id || result.isFetching}
					onClick={() =>
						id &&
						setRequest({
							portfolio: pf,
							netting_set_id: id,
							own_rating: own,
							borrowing_spread: funding / 1e4,
							lending_spread: funding / 1e4,
							paths,
						} as PortfolioXvaRequest)
					}
				>
					{result.isFetching ? "Simulating…" : "Compute xVA"}
				</Button>
			</div>

			{result.error ? (
				<ErrorState error={result.error} onRetry={() => result.refetch()} />
			) : result.isFetching ? (
				<ChartSkeleton />
			) : !data ? (
				<p className="max-w-3xl text-sm leading-relaxed text-ink-secondary">
					The trades held under the netting set are simulated on today&apos;s
					swap curves, with the life they have left and the rates of their
					contracts. The counterparty&apos;s rating, sector and recovery, and
					the collateral agreement, are those of the Counterparties tab.
				</p>
			) : (
				<>
					<XvaResultHeader data={data} />
					<WarningList warnings={data.warnings} />
					{data.market.foreign && (
						<ForeignMarketPanel
							market={data.market.foreign}
							domestic={data.market.currency}
						/>
					)}
					{data.market.equity && (
						<EquityMarketPanel market={data.market.equity} />
					)}
					<Tabs defaultValue="exposure">
						<TabsList>
							<TabsTrigger value="exposure">Exposure</TabsTrigger>
							<TabsTrigger value="adjustments">Adjustments</TabsTrigger>
							<TabsTrigger value="capital">Margin and capital</TabsTrigger>
						</TabsList>
						<TabsContent value="exposure">
							<ExposurePanel data={data} />
						</TabsContent>
						<TabsContent value="adjustments">
							<div className="flex flex-col gap-6">
								<AdjustmentsPanel data={data} />
								<TradesTable data={data} />
							</div>
						</TabsContent>
						<TabsContent value="capital">
							<MarginCapitalPanel data={data} />
						</TabsContent>
					</Tabs>
					<Methodology
						id="portfolio-xva-methodology"
						title="xVA — methodology"
						sections={data.methodology}
					/>
				</>
			)}
		</div>
	);
}
