import { ApiError } from "@/shared/api/errors";

/** What went wrong, in the heading: only a 4xx other than a timeout is the
 * script's (or its inputs') fault. A timeout said "Script rejected" over a
 * valid script that the server could not price in time. */
function heading(error: Error): string {
	if (error instanceof ApiError) {
		if (error.status === 408 || error.kind === "timeout")
			return "Pricing timed out";
		if (error.kind === "network") return "Server unreachable";
		if (error.status >= 500) return "Pricing failed on the server";
	}
	return "Script rejected";
}

/** The parser's / pricer's refusal, shown under the form. Pulled out of
 * ScriptingPreview to keep that page under the feature-file line cap. */
export function ScriptRejected({ error }: { error: Error }) {
	return (
		<div
			role="alert"
			className="rounded-md border border-critical/40 bg-critical/5 p-4"
		>
			<p className="mb-2 text-sm font-medium text-critical">{heading(error)}</p>
			<pre className="overflow-x-auto font-mono text-2xs whitespace-pre-wrap text-critical">
				{error.message}
			</pre>
		</div>
	);
}
