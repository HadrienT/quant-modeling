import type { RatesOverviewResponse } from "@/shared/api";
import { Methodology } from "@/shared/ui";

/**
 * The rates methodology, served with the data (api/app/rates.py), next to
 * the code that computes it.
 */
export function RatesMethodology({
	sections,
}: {
	sections: RatesOverviewResponse["methodology"];
}) {
	return <Methodology id="rates-methodology" sections={sections} />;
}
