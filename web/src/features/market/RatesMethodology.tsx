import type { RatesOverviewResponse } from "@/shared/api";

/**
 * The methodology, always visible (not behind a toggle): where every number
 * comes from, how each derived curve is computed, and what is not shown and
 * why. The text is served with the data (api/app/rates.py), next to the code
 * that computes it.
 */
export function RatesMethodology({
	sections,
}: {
	sections: RatesOverviewResponse["methodology"];
}) {
	return (
		<section
			aria-labelledby="rates-methodology"
			className="flex flex-col gap-4 rounded-sm border border-hairline bg-surface p-4"
		>
			<h2 id="rates-methodology" className="text-sm font-semibold text-ink">
				Methodology
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
