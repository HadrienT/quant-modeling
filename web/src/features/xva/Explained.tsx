import { type ReactNode } from "react";
import { Formula } from "@/shared/products/Formula";

/**
 * A step of the calculation, self-explanatory: what it is, its formula, and
 * where it comes from, next to the figures it produced. The page is meant to
 * be read without the methodology open in another tab.
 */
export function Explained({
	title,
	tex,
	source,
	children,
	figures,
}: {
	title: string;
	tex?: string;
	source: string;
	/** The explanation, in prose. */
	children: ReactNode;
	/** The numbers this step produced. */
	figures?: ReactNode;
}) {
	return (
		<div className="flex flex-col gap-2 rounded-sm border border-hairline bg-surface p-3">
			<div className="flex flex-wrap items-baseline justify-between gap-2">
				<h3 className="text-sm font-semibold text-ink">{title}</h3>
				{figures}
			</div>
			<p className="text-sm leading-relaxed text-ink-secondary">{children}</p>
			{tex && <Formula tex={tex} block />}
			<p className="text-2xs text-ink-muted">Source: {source}</p>
		</div>
	);
}

/** A labelled figure with its Monte-Carlo error underneath. */
export function Figure({
	label,
	value,
	note,
}: {
	label: string;
	value: string;
	note?: string;
}) {
	return (
		<div className="flex flex-col rounded-sm border border-hairline bg-surface px-3 py-2">
			<span className="text-2xs text-ink-muted">{label}</span>
			<span className="text-base font-medium text-ink tabular-nums">
				{value}
			</span>
			{note && <span className="text-2xs text-ink-muted">{note}</span>}
		</div>
	);
}
