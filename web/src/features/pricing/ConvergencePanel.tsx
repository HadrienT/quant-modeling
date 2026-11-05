import {
	type PricingPath,
	type PricingResult,
	convergencePath,
	useConvergence,
} from "@/shared/api";
import { ConvergenceChart } from "@/shared/viz";
import { type MeasuredPoint, fitSlope, slopeLine } from "./convergenceSlope";

/**
 * The Monte-Carlo convergence of the pricing above, measured: the server
 * prices the product again at N/32 … N/2 paths, each point with its own seed
 * (api/app/convergence.py), and the pricing above is the last point, at N.
 * It is not priced twice: the study costs just under one pricing more.
 */
export function ConvergencePanel({
	endpoint,
	body,
	priced,
}: {
	endpoint: PricingPath | null;
	body: unknown;
	priced: PricingResult;
}) {
	const path = convergencePath(endpoint);
	const q = useConvergence({ path, body });
	if (path === null) return null;
	const points: MeasuredPoint[] | undefined = q.data && [
		...q.data.points,
		{
			paths: q.data.n_paths,
			npv: priced.npv,
			std_error: priced.mc_std_error,
			compute_ms: priced.compute_ms,
		},
	];
	return (
		<div className="flex flex-col gap-1">
			<ConvergenceChart
				title="Monte-Carlo convergence (measured)"
				subtitle={points ? slopeLine(fitSlope(points)) : undefined}
				isLoading={q.isLoading}
				error={q.error}
				series={
					points && [
						{
							label: "standard error",
							points: points.map((p) => ({
								paths: p.paths,
								error: p.std_error,
							})),
						},
					]
				}
				table={
					points && {
						columns: ["Paths", "Seed", "Estimate", "Std error", "Time (ms)"],
						rows: points.map((p) => [
							p.paths,
							p.seed ?? "pricing above",
							p.npv.toPrecision(8),
							p.std_error.toExponential(3),
							p.compute_ms == null ? "—" : p.compute_ms.toFixed(1),
						]),
					}
				}
			/>
			<p className="text-2xs text-ink-muted">
				Each point is a separate pricing by the same engine and estimator, with
				its own seed; the last one is the price above. The dashed line is the
				1/√N law of plain Monte Carlo, drawn from the first point. The slope is
				the least-squares fit of ln(std error) on ln(paths), ± its regression
				standard error. The study costs just under one pricing more.
			</p>
		</div>
	);
}
