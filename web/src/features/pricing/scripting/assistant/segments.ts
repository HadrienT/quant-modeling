export type Segment = { kind: "text" | "code"; body: string };

/**
 * Split an assistant reply into prose and ```qms code blocks. Tolerates the
 * reply being mid-stream: an unterminated fence is shown as code as soon as it
 * opens, so a script types itself out in a code box instead of flashing as
 * prose and jumping.
 */
export function splitSegments(text: string): Segment[] {
	const out: Segment[] = [];
	const fence = /```[A-Za-z]*[ \t]*\n/g;
	let cursor = 0;
	for (;;) {
		fence.lastIndex = cursor;
		const open = fence.exec(text);
		if (!open) break;
		if (open.index > cursor) {
			out.push({ kind: "text", body: text.slice(cursor, open.index) });
		}
		const start = open.index + open[0].length;
		const close = text.indexOf("```", start);
		if (close === -1) {
			out.push({ kind: "code", body: text.slice(start) });
			return out.filter((s) => s.body.trim() !== "");
		}
		out.push({
			kind: "code",
			body: text.slice(start, close).replace(/\n$/, ""),
		});
		cursor = close + 3;
	}
	if (cursor < text.length)
		out.push({ kind: "text", body: text.slice(cursor) });
	return out.filter((s) => s.body.trim() !== "");
}
