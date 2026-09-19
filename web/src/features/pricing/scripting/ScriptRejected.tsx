/** The parser's / pricer's refusal, shown under the form. Pulled out of
 * ScriptingPreview to keep that page under the feature-file line cap. */
export function ScriptRejected({ message }: { message: string }) {
	return (
		<div
			role="alert"
			className="rounded-md border border-critical/40 bg-critical/5 p-4"
		>
			<p className="mb-2 text-sm font-medium text-critical">Script rejected</p>
			<pre className="overflow-x-auto font-mono text-2xs whitespace-pre-wrap text-critical">
				{message}
			</pre>
		</div>
	);
}
