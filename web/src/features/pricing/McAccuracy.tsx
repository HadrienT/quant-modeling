import type { PricingResult } from "@/shared/api";
import { formatNumber } from "@/shared/format";
import { DiscretisationBias } from "@/shared/ui/density";
import { TIME_LIMIT } from "./scripting/ModelWarnings";

/** What stopped a pricing to a precision (PrecisionOutcome.stopped). */
const STOPPED: Record<string, string> = {
	target_reached: "target reached",
	out_of_budget: "target out of budget",
	bias_floor:
		"time-step bias floor: finer steps or multilevel needed, not paths",
	time_budget: "time budget used before the target",
};

/**
 * A Monte-Carlo price's accuracy beyond its ± (issue 331): the time-step
 * bias with its own error, and, when priced to a precision, the target, the
 * rule that ended the run and what the target would have needed. A run the
 * time limit cut short (fewer paths than asked, or a precision target not
 * reached) says so first, as a warning.
 */
export function McAccuracy({ r }: { r: PricingResult }) {
	const p = r.precision;
	const rel = (x: number) =>
		r.npv ? ` (${((x / Math.abs(r.npv)) * 100).toFixed(3)} %)` : "";
	const limited = (r.warnings ?? []).filter((w) =>
		w.code.startsWith(TIME_LIMIT),
	);
	return (
		<>
			{limited.map((w) => (
				<p
					key={w.code}
					role="alert"
					className="rounded-md border border-warning/40 bg-warning/5 p-2 text-xs text-warning"
				>
					{w.message}
				</p>
			))}
			<DiscretisationBias
				bias={r.discretisation_bias}
				error={r.discretisation_bias_std_error}
				value={r.npv}
			/>
			{p && (
				<span
					className="block font-mono text-2xs text-ink-muted"
					title={
						p.note ||
						"Pilot run, then the paths the target needs, stopped at the first rule met"
					}
				>
					Precision target {formatNumber(p.target, "price")}
					{rel(p.target)}
					{p.mode === "market" &&
						p.delta_sigma != null &&
						` = k × vega × half-spread ${(p.delta_sigma * 100).toFixed(3)} vol pt at ${p.expiry?.toFixed(2)}y`}
					: {STOPPED[p.stopped] ?? p.stopped}, error{" "}
					{formatNumber(p.achieved_error, "price")} with{" "}
					{p.paths.toLocaleString("en-US")} paths
					{p.stopped === "out_of_budget" &&
						p.paths_needed != null &&
						` (the target needed ~${p.paths_needed.toLocaleString("en-US")})`}
					, {p.seconds.toFixed(1)} s
				</span>
			)}
		</>
	);
}
