import { AlertTriangle } from "lucide-react";

/**
 * The methodology of a data page, always visible (not behind a toggle): where
 * every number comes from and how each derived one is computed. The text is
 * served with the data, by the API module that computes it, so the page
 * cannot describe a computation it does not do.
 */
export function Methodology({
	sections,
	id,
	title = "Methodology",
}: {
	sections: { title: string; paragraphs: string[] }[];
	id: string;
	title?: string;
}) {
	return (
		<section
			aria-labelledby={id}
			className="flex flex-col gap-4 rounded-sm border border-hairline bg-surface p-4"
		>
			<h2 id={id} className="text-sm font-semibold text-ink">
				{title}
			</h2>
			{sections.map((s) => (
				<div key={s.title} className="flex flex-col gap-1.5">
					<h3 className="text-xs font-semibold tracking-wide text-ink-secondary uppercase">
						{s.title}
					</h3>
					{s.paragraphs.map((p) => (
						<p key={p} className="text-sm leading-relaxed text-ink-secondary">
							{p}
						</p>
					))}
				</div>
			))}
		</section>
	);
}

/** Data warnings (staleness, missing inputs), shown above what they qualify. */
export function WarningList({ warnings }: { warnings: string[] }) {
	if (warnings.length === 0) return null;
	return (
		<ul className="flex flex-col gap-1 rounded-sm border border-warning/40 bg-surface p-3 text-xs text-warning">
			{warnings.map((w) => (
				<li key={w} className="flex gap-2">
					<AlertTriangle className="size-3.5 shrink-0" aria-hidden />
					{w}
				</li>
			))}
		</ul>
	);
}
