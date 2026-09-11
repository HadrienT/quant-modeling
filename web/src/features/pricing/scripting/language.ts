import { HighlightStyle, StreamLanguage } from "@codemirror/language";
import { tags as t } from "@lezer/highlight";
import { type EditorView } from "@codemirror/view";
import {
	type Completion,
	type CompletionSource,
} from "@codemirror/autocomplete";

/**
 * A CodeMirror "stream" tokenizer for the payoff scripting language
 * (blueprint/wp/16-scripting.md §1.2). This mirrors the C++ lexer's lexical
 * classes closely enough for syntax highlighting — it does not need to be a
 * full grammar (no nesting, no precedence): a stream tokenizer just needs to
 * say what kind of token starts at the cursor.
 */

const KEYWORDS = new Set([
	"if",
	"then",
	"else",
	"endif",
	"pays",
	"and",
	"or",
	"not",
]);
const FUNCTIONS = new Set([
	"min",
	"max",
	"log",
	"exp",
	"sqrt",
	"abs",
	"smooth",
	"spot",
]);

/** Legacy CodeMirror token names -> Lezer highlight tags, used by both the
 * tokenizer (via `tokenTable`) and `scriptingHighlightStyle` below. */
const tokenTable = {
	keyword: t.keyword,
	function: t.function(t.variableName),
	date: t.number,
	number: t.number,
	comment: t.lineComment,
	variableName: t.variableName,
	compareOperator: t.compareOperator,
	arithmeticOperator: t.arithmeticOperator,
	punctuation: t.punctuation,
};

export const scriptingLanguage = StreamLanguage.define({
	token(stream) {
		if (stream.eatSpace()) return null;
		if (stream.match("#")) {
			stream.skipToEnd();
			return "comment";
		}
		if (stream.match(/^\d{4}-\d{2}-\d{2}/)) return "date";
		if (stream.match(/^\d+(\.\d+)?/)) return "number";
		if (stream.match(/^[A-Za-z_][A-Za-z0-9_]*/)) {
			const word = stream.current().toLowerCase();
			if (KEYWORDS.has(word)) return "keyword";
			if (FUNCTIONS.has(word)) return "function";
			return "variableName";
		}
		if (stream.match(/^(<=|>=|!=|=)/)) return "compareOperator";
		if (stream.match(/^[<>]/)) return "compareOperator";
		if (stream.match(/^[+\-*/^]/)) return "arithmeticOperator";
		if (stream.match(/^[(),]/)) return "punctuation";
		stream.next();
		return null;
	},
	tokenTable,
});

/** Colours reuse the app's own design tokens (theme.css) — the editor follows
 * light/dark like the rest of the page, and the palette is the same one the
 * charts use, not a bespoke "code editor" theme. */
export const scriptingHighlightStyle = HighlightStyle.define([
	{ tag: t.keyword, color: "var(--color-accent)", fontWeight: 600 },
	{ tag: t.function(t.variableName), color: "var(--color-series-7)" },
	{ tag: t.number, color: "var(--color-series-4)" },
	{ tag: t.lineComment, color: "var(--color-ink-muted)", fontStyle: "italic" },
	{ tag: t.variableName, color: "var(--color-ink)" },
	{ tag: t.compareOperator, color: "var(--color-series-3)" },
	{ tag: t.arithmeticOperator, color: "var(--color-ink-secondary)" },
	{ tag: t.punctuation, color: "var(--color-ink-secondary)" },
]);

/**
 * "Enter" carries the current line's indentation forward, and adds one level
 * after a date header or a `then` / `else` — the two places the language
 * expects an indented block. Not a full indent engine (the language doesn't
 * need one: `if / then / else / endIf` is delimited by keywords, not
 * indentation — see lexer.hpp), just enough that typing a script doesn't
 * fight the editor.
 */
export function scriptingSmartNewline(view: EditorView): boolean {
	const { state } = view;
	const line = state.doc.lineAt(state.selection.main.from);
	const leadingWs = /^[ \t]*/.exec(line.text)?.[0] ?? "";
	const trimmed = line.text.trim();
	const isDateHeader = /^\d{4}-\d{2}-\d{2}(\s+\d{4}-\d{2}-\d{2})*$/.test(
		trimmed,
	);
	const opensBlock =
		isDateHeader || /\bthen$/i.test(trimmed) || /^else$/i.test(trimmed);
	const indent = opensBlock ? `${leadingWs}    ` : leadingWs;
	view.dispatch(
		state.update(state.replaceSelection(`\n${indent}`), {
			scrollIntoView: true,
		}),
	);
	return true;
}

const KEYWORD_COMPLETIONS: Completion[] = [
	{ label: "if", type: "keyword", detail: "if <cond> then … [else …] endIf" },
	{ label: "then", type: "keyword" },
	{ label: "else", type: "keyword" },
	{ label: "endIf", type: "keyword" },
	{
		label: "pays",
		type: "keyword",
		detail: "pays <expr> — record a cash flow",
	},
	{ label: "and", type: "keyword" },
	{ label: "or", type: "keyword" },
	{ label: "not", type: "keyword" },
];

const FUNCTION_COMPLETIONS: Completion[] = [
	{ label: "spot()", type: "function", detail: "the underlying's level" },
	{ label: "min(", type: "function", detail: "min(a, b)" },
	{ label: "max(", type: "function", detail: "max(a, b)" },
	{ label: "log(", type: "function", detail: "log(x)" },
	{ label: "exp(", type: "function", detail: "exp(x)" },
	{ label: "sqrt(", type: "function", detail: "sqrt(x)" },
	{ label: "abs(", type: "function", detail: "abs(x)" },
	{ label: "smooth(", type: "function", detail: "smooth(x, half-width)" },
];

/** Keyword / function completion — a static list is enough for a language
 * this small; no need for an LSP round-trip. */
export const scriptingCompletions: CompletionSource = (context) => {
	const word = context.matchBefore(/[A-Za-z_][A-Za-z0-9_]*/);
	if (!word || (word.from === word.to && !context.explicit)) return null;
	return {
		from: word.from,
		options: [...KEYWORD_COMPLETIONS, ...FUNCTION_COMPLETIONS],
	};
};
