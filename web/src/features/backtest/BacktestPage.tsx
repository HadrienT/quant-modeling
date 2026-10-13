import { useNavigate, useSearch } from "@tanstack/react-router";
import { useBacktestStrategy, useBacktestUniverse } from "@/shared/api";
import {
	Disclaimer,
	Tabs,
	TabsContent,
	TabsList,
	TabsTrigger,
} from "@/shared/ui";
import { ChartSkeleton, ErrorState } from "@/shared/ui/states";
import { BacktestControls } from "./BacktestControls";
import { CostPanel } from "./CostPanel";
import { DataPanel } from "./DataPanel";
import { InferencePanel } from "./InferencePanel";
import { PerformancePanel } from "./PerformancePanel";
import { ValidationPanel } from "./ValidationPanel";
import {
	type BacktestView,
	decodeView,
	encodeView,
	strategyRequest,
	validationRequest,
} from "./state";

const TABS = [
	"performance",
	"costs",
	"inference",
	"validation",
	"data",
] as const;
type Tab = (typeof TABS)[number];

/**
 * Backtest — what an allocation strategy would have earned, and how much of
 * that the data can vouch for. The engine is the quant-backtest library: a
 * book in shares and cash on prices as quoted, dividends and splits applied
 * on their date, costs paid in cash. This page asks and shows; it computes
 * nothing. Every figure comes with its interval and its convention, net of
 * costs next to gross, always against the same comparison points. The
 * configuration lives in the URL, so a backtest is a link.
 */
export default function BacktestPage() {
	const search = useSearch({ from: "/backtest" });
	const navigate = useNavigate({ from: "/backtest" });
	const view = decodeView(search.c);
	const tab: Tab = TABS.find((t) => t === search.tab) ?? "performance";

	const universe = useBacktestUniverse();
	const last = universe.data?.last ?? "";
	const result = useBacktestStrategy(
		universe.data ? strategyRequest(view, last) : null,
	);
	const data = result.data;

	return (
		<div className="mx-auto flex max-w-6xl flex-col gap-4">
			<div className="flex flex-col gap-1">
				<h1 className="text-lg font-semibold text-ink">Backtest</h1>
				<p className="text-sm text-ink-secondary">
					What an allocation strategy would have earned on real prices, after
					costs, against the portfolios that need no estimate — and how much of
					the difference the data can actually tell apart.
				</p>
				<Disclaimer className="mt-1" />
			</div>
			{universe.error ? (
				<ErrorState error={universe.error} onRetry={() => universe.refetch()} />
			) : !universe.data ? (
				<ChartSkeleton />
			) : (
				<BacktestControls
					key={search.c ?? "default"}
					universe={universe.data}
					view={view}
					running={result.isFetching}
					onRun={(next: BacktestView) =>
						navigate({
							search: (prev) => ({ ...prev, c: encodeView(next) }),
						})
					}
				/>
			)}
			{result.error ? (
				<ErrorState error={result.error} onRetry={() => result.refetch()} />
			) : !data ? (
				universe.data && <ChartSkeleton />
			) : (
				<Tabs
					value={tab}
					onValueChange={(v) =>
						navigate({ search: (prev) => ({ ...prev, tab: v as Tab }) })
					}
				>
					<TabsList>
						<TabsTrigger value="performance">Performance</TabsTrigger>
						<TabsTrigger value="costs">Costs</TabsTrigger>
						<TabsTrigger value="inference">Significance</TabsTrigger>
						<TabsTrigger value="validation">Overfitting</TabsTrigger>
						<TabsTrigger value="data">Data and conventions</TabsTrigger>
					</TabsList>
					<TabsContent value="performance">
						<PerformancePanel data={data} />
					</TabsContent>
					<TabsContent value="costs">
						<CostPanel data={data} />
					</TabsContent>
					<TabsContent value="inference">
						<InferencePanel data={data} />
					</TabsContent>
					<TabsContent value="validation">
						<ValidationPanel request={validationRequest(view, last)} />
					</TabsContent>
					<TabsContent value="data">
						<DataPanel data={data} />
					</TabsContent>
				</Tabs>
			)}
		</div>
	);
}
