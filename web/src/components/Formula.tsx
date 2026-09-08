import katex from "katex";

type FormulaProps = {
	/** LaTeX source, without surrounding $ or $$ delimiters. */
	tex: string;
	/** Render as a centered display formula instead of an inline span. */
	block?: boolean;
};

/**
 * Renders a LaTeX string via KaTeX.
 *
 * `throwOnError: false` degrades gracefully to KaTeX's own inline error
 * rendering (red text showing the offending source) instead of crashing
 * the page if a formula has a typo — important since this is fed by a
 * hand-written content registry, not user input.
 */
export default function Formula({ tex, block = false }: FormulaProps) {
	const html = katex.renderToString(tex, {
		throwOnError: false,
		displayMode: block,
	});
	// KaTeX's own trusted output, not user input.
	return (
		<span
			className={block ? "formula-block" : "formula-inline"}
			dangerouslySetInnerHTML={{ __html: html }}
		/>
	);
}
