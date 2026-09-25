import { Button, Field } from "@/shared/ui";
import { Metric, MetricRow } from "@/shared/ui/density";
import { ErrorState } from "@/shared/ui/states";
import type { SimulationCalibrateResponse } from "@/shared/api/types";
import type { UseSimulation } from "./useSimulation";

const pct = (x: number | null | undefined) =>
	x == null ? "—" : `${(x * 100).toFixed(2)}%`;

/** The "calibrate on ticker" form and its resulting diagnostics — split out
 * of SimulationPage.tsx to stay under the per-feature-file line budget.
 * Black-Scholes and SABR read one SVI slice; local vol, Heston and SLV the
 * whole stored surface. */
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

			{calibrated &&
				(calibrated.slice_ttm != null ? (
					<SliceMetrics c={calibrated} />
				) : (
					<SurfaceMetrics c={calibrated} />
				))}
		</div>
	);
}

function SliceMetrics({ c }: { c: SimulationCalibrateResponse }) {
	return (
		<MetricRow>
			<Metric
				label="Slice maturity used"
				value={(c.slice_ttm ?? 0).toFixed(3)}
				unit="y"
			/>
			<Metric label="Forward" value={c.forward.toFixed(2)} />
			{c.model === "black_scholes" ? (
				<Metric label="ATM vol" value={pct(c.vol)} />
			) : (
				<>
					<Metric
						label="alpha / rho / nu"
						value={`${c.alpha?.toFixed(3)} / ${c.rho?.toFixed(3)} / ${c.nu?.toFixed(3)}`}
					/>
					<Metric
						label="Fit RMSE"
						value={c.rmse != null ? c.rmse.toExponential(2) : "—"}
						footnote={c.converged ? "converged" : "did not converge"}
					/>
				</>
			)}
			<Metric
				label="Clean quotes"
				value={c.n_clean_quotes ?? "—"}
				footnote={c.cleaning_summary ?? undefined}
			/>
		</MetricRow>
	);
}

function SurfaceMetrics({ c }: { c: SimulationCalibrateResponse }) {
	const h = c.heston;
	const l = c.leverage;
	return (
		<MetricRow>
			<Metric
				label="Stored surface"
				value={c.snapshot ?? "—"}
				footnote={c.surface ?? undefined}
			/>
			<Metric label="Spot" value={c.spot.toFixed(2)} />
			{h && (
				<>
					<Metric
						label="Heston fit (implied vol)"
						value={`RMSE ${pct(h.iv_rmse)}`}
						footnote={`worst ${pct(h.iv_worst)}, ${h.n_quotes} points on ${h.n_maturities} maturities`}
					/>
					<Metric
						label="Feller (2κθ > ξ²)"
						value={h.feller ? "holds" : "fails"}
						footnote="fails: the variance can reach zero"
					/>
				</>
			)}
			{l && (
				<Metric
					label="SLV leverage"
					value={`${l.min.toFixed(2)} – ${l.max.toFixed(2)}`}
					footnote={`${pct(l.clamped_share)} of the grid at its floor or cap; ${l.n_particles.toLocaleString("en-US")} particles`}
				/>
			)}
		</MetricRow>
	);
}
