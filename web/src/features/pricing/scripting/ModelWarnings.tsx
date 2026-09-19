type Warning = { code: string; severity: "warning" | "info"; message: string };

/** What the script's price depends on that the chosen model cannot capture
 * (scripting/model_advice.hpp) — shown next to the price it qualifies. */
export function ModelWarnings({ warnings }: { warnings: Warning[] }) {
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
