import { useState } from "react";
import {
	type RatesAnalysisRequest,
	useRatesAnalysis,
	useRatesExample,
	useRatesMarket,
} from "@/shared/api";
import { Badge, Button, Methodology } from "@/shared/ui";
import { ChartSkeleton, ErrorState } from "@/shared/ui/states";
import { CalibrationPanel } from "./CalibrationPanel";
import { CurvesPanel } from "./CurvesPanel";
import { MarketQuotesPanel } from "./MarketQuotesPanel";
import { QuotesPanel } from "./QuotesPanel";
import { SwapPanel } from "./SwapPanel";
import { SwaptionPanel } from "./SwaptionPanel";

/**
 * Rates derivatives: from a set of quotes, the OIS and index curves
 * (multi-curve), a swap, a Hull-White model calibrated to swaption vols, and
 * one swaption under Bachelier, Black, SABR and Hull-White with its Bermudan.
 * Two sets of quotes: illustrative EUR ones (no free source publishes dealer
 * quotes) and USD SOFR ones built from the swaps and swaptions actually
 * traded, with the trades behind each number. The page says which it shows
 * and computes whatever the user enters (api/app/rates_derivatives.py).
 */
export default function RatesPage() {
	const example = useRatesExample();
	const [useMarket, setUseMarket] = useState(false);
	const market = useRatesMarket(useMarket);
	const [draft, setDraft] = useState<RatesAnalysisRequest>();
	const [applied, setApplied] = useState<RatesAnalysisRequest>();
	const source = useMarket ? market : example;
	const base = source.data?.request as RatesAnalysisRequest | undefined;
	const current = draft ?? base;
	const request = applied ?? base;
	const analysis = useRatesAnalysis(request);
	const dirty = current !== request;
	const data = analysis.data;
	const edit = (next: RatesAnalysisRequest) => setDraft(next);

	return (
		<div className="mx-auto flex max-w-6xl flex-col gap-6">
			<div className="flex flex-col gap-1">
				<h1 className="text-lg font-semibold text-ink">Rates derivatives</h1>
				<p className="text-sm text-ink-secondary">
					OIS discounting and a projection curve per index, swaps, and swaptions
					under four models, with a Hull-White model calibrated to the swaption
					grid and the Bermudan it prices.
				</p>
				{useMarket && market.data ? (
					<p className="flex flex-wrap items-center gap-2 text-xs text-ink-secondary">
						<Badge tone="good">Traded prices</Badge>
						{market.data.label}
					</p>
				) : (
					!useMarket &&
					example.data && (
						<p className="flex flex-wrap items-center gap-2 text-xs text-warning">
							<Badge>Manual input</Badge>
							{example.data.label}
						</p>
					)
				)}
				<div>
					<Button
						variant="secondary"
						onClick={() => {
							setUseMarket(!useMarket);
							setDraft(undefined);
							setApplied(undefined);
						}}
					>
						{useMarket
							? "Use the illustrative EUR quotes"
							: "Use USD SOFR quotes from traded prices"}
					</Button>
				</div>
			</div>
			{source.error ? (
				<ErrorState error={source.error} onRetry={() => source.refetch()} />
			) : !current ? (
				<ChartSkeleton />
			) : (
				<>
					<div className="flex flex-wrap items-center gap-2">
						<Button
							disabled={!dirty || analysis.isFetching}
							onClick={() => setApplied(current)}
						>
							{analysis.isFetching ? "Computing…" : "Recompute"}
						</Button>
						<Button
							variant="secondary"
							disabled={!draft && !applied}
							onClick={() => {
								setDraft(undefined);
								setApplied(undefined);
							}}
						>
							Reset the quotes
						</Button>
						{dirty && (
							<span className="text-xs text-ink-muted">
								Edited: recompute to update the results.
							</span>
						)}
					</div>
					{useMarket && market.data && (
						<MarketQuotesPanel market={market.data} />
					)}
					<QuotesPanel value={current} onChange={edit} />
					{analysis.error && (
						<ErrorState
							error={analysis.error}
							onRetry={() => analysis.refetch()}
						/>
					)}
					{data && (
						<CurvesPanel
							curves={data.curves}
							indexPeriod={1 / request!.float_frequency}
						/>
					)}
					<SwapPanel
						value={current.swap}
						onChange={(swap) => edit({ ...current, swap })}
						result={data?.swap}
					/>
					{data && <CalibrationPanel hw={data.hull_white} />}
					<SwaptionPanel
						value={current.swaption}
						onChange={(swaption) => edit({ ...current, swaption })}
						result={data?.swaption}
					/>
					{data && (
						<Methodology id="rates-methodology" sections={data.methodology} />
					)}
				</>
			)}
		</div>
	);
}
