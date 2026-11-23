type Warning = { code: string; severity: "warning" | "info"; message: string };

/** The code prefix of the warnings that a time limit cut the run short (the
 * API's time_limit_paths and time_limit_precision), shown by McAccuracy. */
export const TIME_LIMIT = "time_limit";

/** What the script's price depends on that the chosen model cannot capture
 * (scripting/model_advice.hpp) — shown next to the price it qualifies.
 * `skip`: a code prefix shown elsewhere on the page (the time-limit
 * warnings, which McAccuracy shows beside the error they qualify). */
export function ModelWarnings({
	warnings: all,
	skip,
}: {
	warnings: Warning[];
	skip?: string;
}) {
	const warnings = skip ? all.filter((w) => !w.code.startsWith(skip)) : all;
	if (warnings.length === 0) return null;
	return (
		<ul className="flex flex-col gap-2">
			{warnings.map((w) => (
				<li
					key={w.code}
					role={w.severity === "warning" ? "alert" : undefined}
					className={
						w.severity === "warning"
							? "rounded-md border border-warning/40 bg-warning/5 p-3 text-sm text-warning"
							: "rounded-md border border-hairline bg-surface-raised p-3 text-sm text-ink-secondary"
					}
				>
					{w.message}
				</li>
			))}
		</ul>
	);
}
