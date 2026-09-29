import { useState } from "react";
import {
	type RatesAnalysisRequest,
	useRatesAnalysis,
	useRatesExample,
} from "@/shared/api";
import { Badge, Button, Methodology } from "@/shared/ui";
import { ChartSkeleton, ErrorState } from "@/shared/ui/states";
import { CalibrationPanel } from "./CalibrationPanel";
import { CurvesPanel } from "./CurvesPanel";
import { QuotesPanel } from "./QuotesPanel";
import { SwapPanel } from "./SwapPanel";
import { SwaptionPanel } from "./SwaptionPanel";

/**
 * Rates derivatives: from a set of quotes, the OIS and index curves
 * (multi-curve), a swap, a Hull-White model calibrated to swaption vols, and
 * one swaption under Bachelier, Black, SABR and Hull-White with its Bermudan.
 * No free source publishes these quotes: the page starts from illustrative
 * ones, says so, and computes whatever the user enters (api/app/rates_derivatives.py).
 */
export default function RatesPage() {
	const example = useRatesExample();
	const [draft, setDraft] = useState<RatesAnalysisRequest>();
	const [applied, setApplied] = useState<RatesAnalysisRequest>();
	const base = example.data?.request as RatesAnalysisRequest | undefined;
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
				{example.data && (
					<p className="flex flex-wrap items-center gap-2 text-xs text-warning">
						<Badge>Manual input</Badge>
						{example.data.label}
					</p>
				)}
			</div>
			{example.error ? (
				<ErrorState error={example.error} onRetry={() => example.refetch()} />
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
							Reset to the example
						</Button>
						{dirty && (
							<span className="text-xs text-ink-muted">
								Edited: recompute to update the results.
							</span>
						)}
					</div>
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
