import { Button, Field } from "@/shared/ui";
import { Metric, MetricRow } from "@/shared/ui/density";
import { ErrorState } from "@/shared/ui/states";
import type { UseSimulation } from "./useSimulation";

/** The "calibrate on ticker" form and its resulting diagnostics — split out
 * of SimulationPage.tsx to stay under the per-feature-file line budget. */
export function CalibratePanel(sim: UseSimulation) {
	const {
		model,
		ticker,
		setTicker,
		ttm,
		setTtm,
		rate,
		setRate,
		beta,
		setBeta,
		calibrate,
		calibrated,
		runCalibration,
	} = sim;

	return (
		<div className="flex flex-col gap-3 rounded-md border border-hairline bg-canvas p-3">
			<div className="flex flex-wrap items-end gap-3">
				<Field
					label="Ticker"
					value={ticker}
					onChange={(e) => setTicker(e.target.value.toUpperCase())}
				/>
				<Field
					label="Target maturity (years)"
					inputMode="decimal"
					value={ttm}
					onChange={(e) => setTtm(e.target.value)}
				/>
				<Field
					label="Rate %"
					inputMode="decimal"
					value={rate}
					onChange={(e) => setRate(e.target.value)}
				/>
				{model === "sabr" && (
					<Field
						label="Beta (fixed)"
						inputMode="decimal"
						value={beta}
						onChange={(e) => setBeta(e.target.value)}
					/>
				)}
				<Button
					onClick={runCalibration}
					disabled={calibrate.isPending || !ticker}
				>
					{calibrate.isPending ? "Calibrating…" : "Calibrate"}
				</Button>
			</div>

			{calibrate.error && <ErrorState error={calibrate.error} compact />}

			{calibrated && (
				<MetricRow>
					<Metric
						label="Slice maturity used"
						value={calibrated.slice_ttm.toFixed(3)}
						unit="y"
					/>
					<Metric label="Forward" value={calibrated.forward.toFixed(2)} />
					{calibrated.model === "black_scholes" ? (
						<Metric
							label="ATM vol"
							value={
								calibrated.vol != null
									? `${(calibrated.vol * 100).toFixed(2)}%`
									: "—"
							}
						/>
					) : (
						<>
							<Metric
								label="alpha / rho / nu"
								value={`${calibrated.alpha?.toFixed(3)} / ${calibrated.rho?.toFixed(3)} / ${calibrated.nu?.toFixed(3)}`}
							/>
							<Metric
								label="Fit RMSE"
								value={
									calibrated.rmse != null
										? calibrated.rmse.toExponential(2)
										: "—"
								}
								footnote={
									calibrated.converged ? "converged" : "did not converge"
								}
							/>
						</>
					)}
					<Metric
						label="Clean quotes"
						value={calibrated.n_clean_quotes}
						footnote={calibrated.cleaning_summary}
					/>
				</MetricRow>
			)}
		</div>
	);
}
