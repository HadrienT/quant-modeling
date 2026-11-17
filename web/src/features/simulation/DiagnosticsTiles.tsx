import type { DistributionResponse } from "@/shared/api/types";
import { Metric, MetricRow } from "@/shared/ui/density";
import { marketFit, martingaleCheck, pct } from "./distribution";

/**
 * What the paths say in four numbers (WP 29), each with its error: the
 * martingale check, the fit to the market's smile, the up-and-out call
 * against the vanilla on the same paths, and the effort.
 */
export function DiagnosticsTiles({ data }: { data: DistributionResponse }) {
	const mg = martingaleCheck(data);
	const fit = marketFit(data);
	const b = data.barrier;
	const last = data.time_grid.length - 1;
	return (
		<MetricRow className="xl:grid-cols-4">
			<Metric
				label="Martingale check"
				value={`|z| ≤ ${mg.z.toFixed(1)}`}
				footnote={
					<>
						Largest |E[S/N] − S₀e<sup>−qt</sup>| over the {last} dates, in
						standard errors
						{mg.index
							? ` (at t = ${data.time_grid[mg.index]!.toFixed(2)}y)`
							: ""}
						. Above ~3.5 is not noise.
					</>
				}
			/>
			<Metric
				label="Fit to the market smile"
				value={fit ? `${(fit.worst * 100).toFixed(2)} vol pts` : "—"}
				footnote={
					fit
						? `Largest gap at ${fit.time.toFixed(2)}y; ${fit.inside} of ${fit.n} strikes inside the 95% band.`
						: data.market
							? "No quoted maturity among the smiles."
							: "Calibrate on a ticker to compare with its stored surface."
				}
			/>
			<Metric
				label="Up-and-out call"
				value={
					b ? `${b.price.toFixed(3)} ± ${(1.96 * b.stderr).toFixed(3)}` : "—"
				}
				footnote={
					b
						? `Strike ${b.strike.toFixed(1)}, barrier ${b.barrier.toFixed(1)} checked on the grid dates; the call without it: ${b.vanilla_price.toFixed(3)} ± ${(1.96 * b.vanilla_stderr).toFixed(3)}.`
						: undefined
				}
			/>
			<Metric
				label="Monte Carlo"
				value={data.n_paths.toLocaleString("en-US")}
				unit="paths"
				footnote={`${last} dates to ${data.time_grid[last]!.toFixed(2)}y, the model's own steps in between; ATM vol at T ${pct(data.atm_vol[last], 2)}.`}
			/>
		</MetricRow>
	);
}
