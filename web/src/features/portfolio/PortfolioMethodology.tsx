import type { PortfolioSnapshot } from "@/shared/api";
import { Methodology } from "@/shared/ui";

/**
 * How the portfolio is valued: the text is served with the numbers
 * (api/app/portfolio_valuation.py), next to the code computing them.
 */
export function PortfolioMethodology({
	sections,
}: {
	sections: PortfolioSnapshot["methodology"];
}) {
	return <Methodology id="portfolio-methodology" sections={sections} />;
}
