type ValidateResult = {
	events: Array<{ date: string; t: number }>;
	variables: string[];
};

/** What a successful "Validate" resolved to — before spending a Monte-Carlo
 * on it. Kept out of ScriptingPreview to stay under the 230-line cap. */
export function ValidationSummary({ result }: { result: ValidateResult }) {
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
			{result.variables.length > 0 && (
				<p className="mt-1 font-mono text-ink-secondary">
					{result.variables.join(", ")}
				</p>
			)}
		</div>
	);
}
