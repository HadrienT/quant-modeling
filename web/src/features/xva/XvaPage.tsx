import { useNavigate, useSearch } from "@tanstack/react-router";
import { useXvaNettingSet, useXvaPortfolios } from "@/shared/api";
import {
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
	ExposurePanel,
	ForeignMarketPanel,
	MarginCapitalPanel,
	TradesTable,
	XvaResultHeader,
} from "@/shared/xva";
import { SensitivitiesPanel } from "./SensitivitiesPanel";
import {
	DEFAULT_VIEW,
	type XvaView,
	nettingSetRequest,
	sensitivitiesRequest,
} from "./state";
import { XvaControls } from "./XvaControls";

/**
 * xVA — the counterparty risk of a netting set, from the simulated exposure
 * to the adjustments it prices: CVA, DVA, FVA, ColVA, MVA and KVA, with the
 * collateral and the capital behind them, and their sensitivities. A page to
 * learn from: every step carries its explanation, its formula and its source
 * next to the figures, on market data from the project's own database. The
 * portfolio and its terms live in the URL, so a view is a link.
 */
export default function XvaPage() {
	const search = useSearch({ from: "/xva" });
	const navigate = useNavigate({ from: "/xva" });
	const view: XvaView = {
		portfolio: search.portfolio ?? DEFAULT_VIEW.portfolio,
		counterparty: search.cpty ?? DEFAULT_VIEW.counterparty,
		own: search.own ?? DEFAULT_VIEW.own,
		csa: search.csa ?? DEFAULT_VIEW.csa,
		wwr: search.wwr ?? DEFAULT_VIEW.wwr,
		funding: search.funding ?? DEFAULT_VIEW.funding,
		paths: search.paths ?? DEFAULT_VIEW.paths,
	};
	const tab = search.tab ?? "exposure";
	const set = (patch: Partial<XvaView>) =>
		navigate({
			search: (prev) => ({
				...prev,
				...("counterparty" in patch ? { cpty: patch.counterparty } : {}),
				...Object.fromEntries(
					Object.entries(patch).filter(([k]) => k !== "counterparty"),
				),
			}),
		});

	const options = useXvaPortfolios();
	const result = useXvaNettingSet(nettingSetRequest(view));
	const data = result.data;

	return (
		<div className="mx-auto flex max-w-6xl flex-col gap-4">
			<div className="flex flex-col gap-1">
				<h1 className="text-lg font-semibold text-ink">xVA</h1>
				<p className="text-sm text-ink-secondary">
					What the default of a counterparty, funding, collateral and capital
					cost on a set of interest-rate trades — simulated on today&apos;s swap
					curve, swaption volatilities and credit spreads.
				</p>
			</div>
			{options.error ? (
				<ErrorState error={options.error} onRetry={() => options.refetch()} />
			) : (
				options.data && (
					<XvaControls options={options.data} view={view} onChange={set} />
				)
			)}
			{result.error ? (
				<ErrorState error={result.error} onRetry={() => result.refetch()} />
			) : !data ? (
				<ChartSkeleton />
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
					<Tabs
						value={tab}
						onValueChange={(v) =>
							navigate({
								search: (prev) => ({ ...prev, tab: v as typeof tab }),
							})
						}
					>
						<TabsList>
							<TabsTrigger value="exposure">Exposure</TabsTrigger>
							<TabsTrigger value="adjustments">Adjustments</TabsTrigger>
							<TabsTrigger value="capital">Margin and capital</TabsTrigger>
							<TabsTrigger value="sensitivities">Sensitivities</TabsTrigger>
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
						<TabsContent value="sensitivities">
							<SensitivitiesPanel
								key={JSON.stringify(sensitivitiesRequest(view))}
								request={sensitivitiesRequest(view)}
							/>
						</TabsContent>
					</Tabs>
					<Methodology id="xva-methodology" sections={data.methodology} />
				</>
			)}
		</div>
	);
}
