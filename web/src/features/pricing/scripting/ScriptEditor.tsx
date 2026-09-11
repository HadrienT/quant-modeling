import { useEffect, useRef } from "react";
import { EditorState } from "@codemirror/state";
import {
	EditorView,
	highlightActiveLine,
	highlightActiveLineGutter,
	keymap,
	lineNumbers,
} from "@codemirror/view";
import {
	defaultKeymap,
	history,
	historyKeymap,
	indentWithTab,
} from "@codemirror/commands";
import {
	bracketMatching,
	indentOnInput,
	indentUnit,
	syntaxHighlighting,
} from "@codemirror/language";
import {
	autocompletion,
	closeBrackets,
	closeBracketsKeymap,
} from "@codemirror/autocomplete";
import {
	forceLinting,
	linter,
	lintGutter,
	type Diagnostic,
} from "@codemirror/lint";
import {
	scriptingCompletions,
	scriptingHighlightStyle,
	scriptingLanguage,
	scriptingSmartNewline,
} from "./language";

/** A ScriptError's location, parsed from the API's pointed error message. */
export type ScriptDiagnostic = {
	line: number;
	col: number;
	message: string;
} | null;

/**
 * A CodeMirror 6 editor for the payoff scripting language: syntax
 * highlighting, keyword/function completion, and — the point of building a
 * real editor rather than a `<textarea>` — the parser's line/column landing
 * as an inline squiggle instead of a text blob under the form.
 *
 * Uncontrolled by design: CodeMirror owns the DOM and its own undo history;
 * `value` is only pushed in when it changes from *outside* the editor (e.g.
 * picking an example), and `onChange` is read through a ref so the editor
 * isn't torn down and rebuilt on every keystroke.
 */
export function ScriptEditor({
	value,
	onChange,
	diagnostic,
	className,
}: {
	value: string;
	onChange: (value: string) => void;
	diagnostic: ScriptDiagnostic;
	className?: string;
}) {
	const hostRef = useRef<HTMLDivElement | null>(null);
	const viewRef = useRef<EditorView | null>(null);
	const onChangeRef = useRef(onChange);
	onChangeRef.current = onChange;
	const diagnosticRef = useRef(diagnostic);
	diagnosticRef.current = diagnostic;

	useEffect(() => {
		if (!hostRef.current) return;

		const diagnosticsSource = (view: EditorView): Diagnostic[] => {
			const d = diagnosticRef.current;
			if (!d) return [];
			const { doc } = view.state;
			if (d.line < 1 || d.line > doc.lines) return [];
			const line = doc.line(d.line);
			const from = Math.min(line.from + Math.max(0, d.col - 1), line.to);
			const to = Math.min(from + 1, doc.length);
			return [{ from, to, severity: "error", message: d.message }];
		};

		const view = new EditorView({
			parent: hostRef.current,
			state: EditorState.create({
				doc: value,
				extensions: [
					lineNumbers(),
					highlightActiveLine(),
					highlightActiveLineGutter(),
					history(),
					indentUnit.of("    "),
					indentOnInput(),
					bracketMatching(),
					closeBrackets(),
					scriptingLanguage,
					syntaxHighlighting(scriptingHighlightStyle),
					autocompletion({ override: [scriptingCompletions] }),
					linter(diagnosticsSource),
					lintGutter(),
					keymap.of([
						{ key: "Enter", run: scriptingSmartNewline },
						...closeBracketsKeymap,
						...defaultKeymap,
						...historyKeymap,
						indentWithTab,
					]),
					EditorView.updateListener.of((update) => {
						if (update.docChanged)
							onChangeRef.current(update.state.doc.toString());
					}),
					EditorView.theme({
						"&": {
							fontSize: "var(--text-2xs)",
							border: "1px solid var(--color-hairline)",
							borderRadius: "var(--radius-sm)",
							backgroundColor: "var(--color-surface)",
						},
						"&.cm-focused": {
							outline: "2px solid var(--color-focus)",
							outlineOffset: "-1px",
						},
						".cm-content": {
							fontFamily: "var(--font-mono)",
							padding: "8px 0",
							minHeight: "16rem",
						},
						".cm-gutters": {
							backgroundColor: "var(--color-surface)",
							color: "var(--color-ink-muted)",
							borderRight: "1px solid var(--color-hairline)",
						},
						".cm-activeLine": {
							backgroundColor: "var(--color-surface-raised)",
						},
						".cm-activeLineGutter": {
							backgroundColor: "var(--color-surface-raised)",
						},
					}),
				],
			}),
		});
		viewRef.current = view;
		return () => {
			view.destroy();
			viewRef.current = null;
		};
		// Mounts once: `onChange` and `diagnostic` are read through refs above,
		// and `value` is synced by the effect below rather than recreating the
		// editor (which would drop cursor position and undo history).
		// eslint-disable-next-line react-hooks/exhaustive-deps
	}, []);

	useEffect(() => {
		const view = viewRef.current;
		if (!view) return;
		const current = view.state.doc.toString();
		if (current !== value) {
			view.dispatch({
				changes: { from: 0, to: current.length, insert: value },
			});
		}
	}, [value]);

	useEffect(() => {
		if (viewRef.current) forceLinting(viewRef.current);
	}, [diagnostic]);

	return <div ref={hostRef} className={className} />;
}
