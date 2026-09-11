import type { ScriptDiagnostic } from "./ScriptEditor";

/**
 * The API returns a ScriptError's `what()` verbatim as the error message:
 * "line N, col M: reason\n    <source line>\n    <caret>". Pull the location
 * out so the editor can point at the exact spot instead of only showing text.
 */
export function parseScriptDiagnostic(message: string): ScriptDiagnostic {
	const m = /^line (\d+), col (\d+):/.exec(message);
	if (!m) return null;
	return { line: Number(m[1]), col: Number(m[2]), message };
}
