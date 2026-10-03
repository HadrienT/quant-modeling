import { useState } from "react";
import {
	type XvaSensitivitiesRequest,
	useXvaSensitivities,
} from "@/shared/api";
import { Button, Methodology, Segmented, WarningList } from "@/shared/ui";
import { ChartSkeleton, ErrorState } from "@/shared/ui/states";
import { Figure } from "./Explained";
import { signed, tenorLabel } from "./format";
import { SaCvaTable } from "./SaCvaTable";
import { type RiskKey, SensitivityBars } from "./SensitivityBars";

const ADJUSTMENTS: { value: RiskKey; label: string }[] = [
	{ value: "cva", label: "CVA" },
	{ value: "dva", label: "DVA" },
	{ value: "fca", label: "FCA" },
	{ value: "fba", label: "FBA" },
];

/**
 * The sensitivities of the adjustments to every market quote, by adjoint
 * differentiation: one more simulation, run on demand. A portfolio the
 * adjoint does not cover says why instead of showing numbers.
 */
export function SensitivitiesPanel({
	request,
}: {
	request: XvaSensitivitiesRequest;
}) {
	const [asked, setAsked] = useState(false);
	const [adjustment, setAdjustment] = useState<RiskKey>("cva");
	const query = useXvaSensitivities(request, asked);
	const data = query.data;

	if (!asked)
		return (
			<div className="flex flex-col items-start gap-3 rounded-sm border border-hairline bg-surface p-4">
				<p className="text-sm text-ink-secondary">
					How much does each adjustment move when a swap rate, a swaption
					volatility or a credit spread moves? All of them come out of one more
					simulation, read backwards: adjoint differentiation.
				</p>
				<Button onClick={() => setAsked(true)}>
					Compute the sensitivities
				</Button>
			</div>
		);
	if (query.error)
		return <ErrorState error={query.error} onRetry={() => query.refetch()} />;
	if (!data) return <ChartSkeleton />;

	return (
		<div className="flex flex-col gap-5">
			<WarningList warnings={data.warnings} />
			<div className="grid grid-cols-2 gap-2 sm:grid-cols-4">
				<Figure
					label="CVA"
					value={signed(data.adjustments.cva.value)}
					note={`± ${Math.round(data.adjustments.cva.error)}`}
				/>
				<Figure
					label="Cost of every sensitivity"
					value={`${data.cost_ratio.toFixed(1)} valuations`}
					note={`${data.seconds_adjoint.toFixed(2)} s against ${data.seconds_valuation.toFixed(3)} s`}
				/>
				<Figure
					label="By bumping instead"
					value={`${data.bump_valuations} valuations`}
					note={`${data.inputs} inputs, moved both ways`}
				/>
				<Figure
					label="Paths"
					value={data.paths.toLocaleString("en-US")}
					note={`${data.threads} threads`}
				/>
			</div>
			<div className="flex flex-wrap items-center gap-2 text-xs text-ink-secondary">
				<span>Sensitivity of</span>
				<Segmented
					label="Adjustment"
					options={ADJUSTMENTS}
					value={adjustment}
					onChange={setAdjustment}
				/>
				<span className="text-ink-muted">
					per basis point of the quote, ± its Monte-Carlo error
				</span>
			</div>
			<div className="grid gap-6 lg:grid-cols-2">
				<SensitivityBars
					title="Par swap rates"
					risks={data.swap_rates}
					adjustment={adjustment}
					label={(r) => tenorLabel(r.tenor)}
				/>
				<SensitivityBars
					title="Swaption volatilities"
					risks={data.swaption_vols}
					adjustment={adjustment}
					label={(r) => `${tenorLabel(r.expiry)} into ${tenorLabel(r.tenor)}`}
				/>
				<SensitivityBars
					title="Counterparty credit spread"
					risks={data.counterparty_spreads}
					adjustment={adjustment}
					label={(r) => tenorLabel(r.tenor)}
				/>
				<SensitivityBars
					title="Model inputs"
					risks={data.model_risks.filter((r) =>
						r.label.startsWith("Hull-White"),
					)}
					adjustment={adjustment}
					label={(r) => r.label.replace("Hull-White ", "")}
				/>
			</div>
			<SaCvaTable sa={data.sa_cva} />
			<Methodology
				id="xva-sensitivities-methodology"
				title="How the sensitivities are computed"
				sections={data.methodology}
			/>
		</div>
	);
}
