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

/** Up to this many events are listed inline; beyond, they fold away. */
const INLINE_EVENTS = 8;

/** Median gap between consecutive events, in calendar days, and its name. */
function spacing(events: ValidateResult["events"]): string | null {
	if (events.length < 3) return null;
	const gaps = events
		.slice(1)
		.map((e, i) => (e.t - events[i]!.t) * 365.25)
		.sort((a, b) => a - b);
	const d = gaps[Math.floor(gaps.length / 2)]!;
	const name =
		d <= 1.6
			? "daily"
			: d <= 8
				? "weekly"
				: d <= 35
					? "monthly"
					: d <= 100
						? "quarterly"
						: d <= 200
							? "semi-annual"
							: "annual";
	return `median spacing ${d < 10 ? d.toFixed(1) : Math.round(d)} days (${name})`;
}

/** What a successful "Validate" resolved to — before spending a Monte-Carlo
 * on it. A daily schedule has hundreds of events: they are summarised in one
 * line and folded, so the form's buttons and the price stay in view. */
export function ValidationSummary({ result }: { result: ValidateResult }) {
	const a = result.analysis;
	const ev = result.events;
	const depends = [
		a.nonlinear_in_spot && "nonlinear in spot (smile)",
		a.spot_threshold_test && "spot-level trigger (skew)",
		a.path_dependent && "path-dependent (forward smile)",
		a.n_underlyings > 1 && `${a.n_underlyings} underlyings`,
	].filter(Boolean);
	const gap = spacing(ev);
	const dates = (
		<ul className="grid grid-cols-2 gap-x-4 font-mono text-ink-secondary sm:grid-cols-3">
			{ev.map((e) => (
				<li key={e.date}>
					{e.date} <span className="text-ink-muted">t={e.t.toFixed(3)}</span>
				</li>
			))}
		</ul>
	);
	return (
		<div className="flex flex-col gap-1 rounded border border-hairline bg-surface-raised p-3 text-2xs">
			<p className="text-ink-secondary">
				<span className="text-ink">{ev.length} event(s)</span>
				{ev.length > 0 && (
					<>
						{" · "}
						<span className="font-mono">
							{ev[0]!.date} → {ev[ev.length - 1]!.date}
						</span>
					</>
				)}
				{gap && ` · ${gap}`}
			</p>
			{ev.length <= INLINE_EVENTS ? (
				dates
			) : (
				<details>
					<summary className="cursor-pointer text-ink-muted">
						Show all {ev.length} dates
					</summary>
					<div className="mt-1 max-h-40 overflow-y-auto">{dates}</div>
				</details>
			)}
			<p className="text-ink-muted">
				Price depends on:{" "}
				{depends.length ? depends.join(" · ") : "nothing beyond forwards"}
			</p>
			{result.variables.length > 0 && (
				<p className="font-mono break-words text-ink-secondary">
					{result.variables.length} variable(s): {result.variables.join(", ")}
				</p>
			)}
		</div>
	);
}
