import type { ReactNode } from "react";
import ReactMarkdown, { type Components } from "react-markdown";
import remarkGfm from "remark-gfm";
import remarkMath from "remark-math";
import { Formula } from "@/shared/products";
import { normalizeMath } from "./math";

type Kids = { children?: ReactNode };

const asText = (children: ReactNode) =>
	(Array.isArray(children) ? children.join("") : String(children ?? "")).trim();

const HEADING = "mt-1 font-semibold text-ink first:mt-0";

/* Sized for a narrow chat column: headings stay close to the body size, and
 * anything that can be wider than the column scrolls on its own. */
const COMPONENTS: Components = {
	p: ({ children }: Kids) => <p className="leading-relaxed">{children}</p>,
	h1: ({ children }: Kids) => <h3 className={HEADING}>{children}</h3>,
	h2: ({ children }: Kids) => <h3 className={HEADING}>{children}</h3>,
	h3: ({ children }: Kids) => <h4 className={HEADING}>{children}</h4>,
	h4: ({ children }: Kids) => <h5 className={HEADING}>{children}</h5>,
	h5: ({ children }: Kids) => <h6 className={HEADING}>{children}</h6>,
	h6: ({ children }: Kids) => <h6 className={HEADING}>{children}</h6>,
	strong: ({ children }: Kids) => (
		<strong className="font-semibold">{children}</strong>
	),
	ul: ({ children }: Kids) => (
		<ul className="flex list-disc flex-col gap-1 pl-5">{children}</ul>
	),
	ol: ({ children }: Kids) => (
		<ol className="flex list-decimal flex-col gap-1 pl-5">{children}</ol>
	),
	li: ({ children }: Kids) => (
		<li className="leading-relaxed marker:text-ink-muted">{children}</li>
	),
	blockquote: ({ children }: Kids) => (
		<blockquote className="border-l-2 border-hairline pl-3 text-ink-secondary">
			{children}
		</blockquote>
	),
	hr: () => <hr className="border-hairline" />,
	a: ({ href, children }: Kids & { href?: string }) => (
		<a
			href={href}
			target="_blank"
			rel="noreferrer noopener"
			className="text-accent underline underline-offset-2"
		>
			{children}
		</a>
	),
	table: ({ children }: Kids) => (
		<div className="overflow-x-auto">
			<table className="w-full border-collapse text-left text-2xs">
				{children}
			</table>
		</div>
	),
	th: ({ children }: Kids) => (
		<th className="border-b border-hairline px-2 py-1 font-semibold text-ink-secondary">
			{children}
		</th>
	),
	td: ({ children }: Kids) => (
		<td className="border-b border-hairline px-2 py-1 align-top">{children}</td>
	),
	// remark-math hands a formula over as <code class="math-…">; the <pre>
	// around a block is dropped so that `code` alone decides what to draw.
	pre: ({ children }: Kids) => <>{children}</>,
	code: ({ className, children }: Kids & { className?: string }) => {
		if (className?.includes("math-display"))
			return <Formula tex={asText(children)} block />;
		if (className?.includes("math-inline"))
			return <Formula tex={asText(children)} />;
		if (String(children ?? "").includes("\n"))
			return (
				<pre className="overflow-x-auto rounded-sm border border-hairline bg-surface-raised p-2 font-mono text-2xs">
					{children}
				</pre>
			);
		return (
			<code className="rounded-sm bg-surface-raised px-1 py-0.5 font-mono text-[0.85em]">
				{children}
			</code>
		);
	},
};

/** Prose of an assistant reply: Markdown (GFM) with LaTeX formulas. Raw HTML in
 * the reply is not interpreted — react-markdown shows it as text. */
export function Markdown({ text }: { text: string }) {
	return (
		<div className="flex min-w-0 flex-col gap-2 break-words [&_.katex-display]:my-1">
			<ReactMarkdown
				remarkPlugins={[remarkGfm, remarkMath]}
				components={COMPONENTS}
			>
				{normalizeMath(text)}
			</ReactMarkdown>
		</div>
	);
}
