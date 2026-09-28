export type Sampler = "pseudo" | "sobol" | "stratified";

/** How a script's Monte-Carlo reduces its variance (blueprint WP 19 §2.5). */
export type VarianceReduction = {
	sampler: Sampler;
	controlVariate: boolean;
	antithetic: boolean;
	importance: boolean;
};

export const DEFAULT_VARIANCE_REDUCTION: VarianceReduction = {
	sampler: "pseudo",
	controlVariate: false,
	antithetic: true,
	importance: false,
};

/** The request fields of these choices. */
export function varianceReductionRequest(v: VarianceReduction) {
	return {
		sampler: v.sampler,
		control_variate: v.controlVariate,
		antithetic: v.antithetic,
		importance_sampling: v.importance,
	};
}

const CHECKS: {
	key: "controlVariate" | "antithetic" | "importance";
	label: string;
	title: string;
}[] = [
	{
		key: "controlVariate",
		label: "Spot control variates",
		title:
			"Regresses the discounted spot at up to 8 event dates (known means) out of the payoff; the diagnostics give the variance removed.",
	},
	{
		key: "antithetic",
		label: "Antithetic pairs",
		title:
			"Pairs each path with its mirror. The pairs already cancel what is linear in the spot: the control variates do most without them.",
	},
	{
		key: "importance",
		label: "Importance sampling",
		title:
			"Drifts the paths toward where the payoff pays (rare events: a deep out-of-the-money digital) and weights them back; kept only when a pilot shows the variance falls.",
	},
];

/** Sampler and variance-reduction choices of the scripting page. */
export function VarianceReductionFields({
	value,
	onChange,
}: {
	value: VarianceReduction;
	onChange: (v: VarianceReduction) => void;
}) {
	return (
		<>
			<label className="flex flex-col gap-1 text-sm">
				<span className="text-2xs text-ink-muted uppercase">Sampler</span>
				<select
					className="rounded border border-hairline bg-surface px-2 py-1"
					value={value.sampler}
					onChange={(e) =>
						onChange({ ...value, sampler: e.target.value as Sampler })
					}
				>
					<option value="pseudo">Pseudo-random</option>
					<option value="sobol">Sobol RQMC + bridge</option>
					<option value="stratified">Stratified W(T)</option>
				</select>
			</label>
			{CHECKS.map((c) => (
				<label
					key={c.key}
					className="flex items-center gap-2 text-sm"
					title={c.title}
				>
					<input
						type="checkbox"
						checked={value[c.key]}
						onChange={(e) => onChange({ ...value, [c.key]: e.target.checked })}
					/>
					{c.label}
				</label>
			))}
		</>
	);
}
