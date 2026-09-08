import { useEffect, useState } from "react";

/**
 * KaTeX renderer — blueprint WP 99. KaTeX (~270 kB of CSS + fonts) is loaded
 * LAZILY: most visitors never open a doc card. `throwOnError: false` degrades to
 * KaTeX's inline error rendering rather than crashing on a typo in the registry.
 */
export function Formula({
	tex,
	block = false,
}: {
	tex: string;
	block?: boolean;
}) {
	const [html, setHtml] = useState<string | null>(null);

	useEffect(() => {
		let alive = true;
		Promise.all([import("katex"), import("katex/dist/katex.min.css")]).then(
			([katex]) => {
				if (!alive) return;
				setHtml(
					katex.default.renderToString(tex, {
						throwOnError: false,
						displayMode: block,
					}),
				);
			},
		);
		return () => {
			alive = false;
		};
	}, [tex, block]);

	if (html == null) {
		return <span className="font-mono text-2xs text-ink-muted">{tex}</span>;
	}
	return (
		<span
			className={block ? "my-1 block overflow-x-auto text-center" : "inline"}
			// KaTeX's own trusted output, not user input.
			dangerouslySetInnerHTML={{ __html: html }}
		/>
	);
}
