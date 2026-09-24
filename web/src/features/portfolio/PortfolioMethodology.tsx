import type { PortfolioSnapshot } from "@/shared/api";

/**
 * How the portfolio is valued, always visible: the text is served with the
 * numbers (api/app/portfolio_valuation.py), next to the code computing them.
 */
export function PortfolioMethodology({
	sections,
}: {
	sections: PortfolioSnapshot["methodology"];
}) {
	return (
		<section
			aria-labelledby="portfolio-methodology"
			className="flex flex-col gap-4 rounded-sm border border-hairline bg-surface p-4"
		>
			<h2 id="portfolio-methodology" className="text-sm font-semibold text-ink">
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
