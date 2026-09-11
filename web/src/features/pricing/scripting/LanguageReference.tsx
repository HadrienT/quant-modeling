/**
 * The scripting language in one screen — so using it doesn't require having
 * read blueprint/wp/16-scripting.md first. Kept in sync by hand with
 * examples/scripting/README.md's "language in one minute" section.
 */
const ROWS: Array<{ term: string; body: string }> = [
	{ term: "spot()", body: "the underlying's level at the current event date." },
	{
		term: "name = expr",
		body: "assign a variable. Variables persist from one event to the next — this is how path-dependency is expressed.",
	},
	{
		term: "pays expr",
		body: "record a cash flow at the current date; it is discounted by that date's numeraire automatically.",
	},
	{
		term: "if cond then … [else …] endIf",
		body: "conditions use = != < <= > >= and and / or / not.",
	},
	{
		term: "min max log exp sqrt abs smooth",
		body: "built-in functions. Operators: + - * / ^ (^ is right-associative).",
	},
	{ term: "# comment", body: "runs to the end of the line." },
	{
		term: "date1  date2  …",
		body: "a date line may list several dates — the same block then applies to each.",
	},
];

export function LanguageReference() {
	return (
		<div className="flex flex-col gap-3 rounded-md border border-hairline bg-surface p-4 text-sm">
			<p className="text-2xs text-ink-muted uppercase">Language reference</p>
			<dl className="flex flex-col gap-2">
				{ROWS.map((row) => (
					<div key={row.term}>
						<dt className="font-mono text-2xs text-ink">{row.term}</dt>
						<dd className="text-2xs text-ink-secondary">{row.body}</dd>
					</div>
				))}
			</dl>
			<p className="text-2xs text-ink-muted">
				Every event must fall strictly after the valuation date — historical
				fixings aren't supported yet.
			</p>
		</div>
	);
}
