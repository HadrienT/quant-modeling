type ValidateResult = {
	events: Array<{ date: string; t: number }>;
	variables: string[];
	analysis: {
		n_underlyings: number;
		nonlinear_in_spot: boolean;
		spot_threshold_test: boolean;
		path_dependent: boolean;
	};
};

/** What a successful "Validate" resolved to — before spending a Monte-Carlo
 * on it. Kept out of ScriptingPreview to stay under the 230-line cap. */
export function ValidationSummary({ result }: { result: ValidateResult }) {
	const a = result.analysis;
	const depends = [
		a.nonlinear_in_spot && "nonlinear in spot (smile)",
		a.spot_threshold_test && "spot-level trigger (skew)",
		a.path_dependent && "path-dependent (forward smile)",
		a.n_underlyings > 1 && `${a.n_underlyings} underlyings`,
	].filter(Boolean);
	return (
		<div className="rounded border border-hairline bg-surface-raised p-3 text-2xs">
			<p className="mb-1 text-ink-muted uppercase">
				{result.events.length} event(s), {result.variables.length || "no"}{" "}
				variable(s)
			</p>
			<ul className="font-mono text-ink-secondary">
				{result.events.map((e) => (
					<li key={e.date}>
						{e.date} — t = {e.t.toFixed(4)}y
					</li>
				))}
			</ul>
			<p className="mt-1 text-ink-muted">
				Price depends on:{" "}
				{depends.length ? depends.join(" · ") : "nothing beyond forwards"}
			</p>
			{result.variables.length > 0 && (
				<p className="mt-1 font-mono text-ink-secondary">
					{result.variables.join(", ")}
				</p>
			)}
		</div>
	);
}
