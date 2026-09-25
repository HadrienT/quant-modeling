const ISO = /\b\d{4}-\d{2}-\d{2}\b/g;
const DAY = 86_400_000;

const toMs = (iso: string) => Date.parse(`${iso}T00:00:00Z`);
const toIso = (ms: number) => new Date(ms).toISOString().slice(0, 10);

/**
 * Moves every date of a library script by the same whole number of weeks so
 * that its first date falls `leadDays` to `leadDays + 6` days after the
 * valuation date: a product from the library always starts "now", however
 * old the file is. Whole weeks keep each date on its weekday, so a literal
 * business day stays one and a schedule keeps its shape. Dates in comments
 * move too, so the explanations stay true.
 */
export function alignToValuationDate(
	script: string,
	valuationDate: string,
	leadDays = 7,
): string {
	const dates = script.match(ISO)?.map(toMs).filter(Number.isFinite) ?? [];
	const valuation = toMs(valuationDate);
	if (dates.length === 0 || !Number.isFinite(valuation)) return script;
	const first = Math.min(...dates);
	const weeks = Math.ceil((valuation + leadDays * DAY - first) / (7 * DAY));
	if (weeks === 0) return script;
	return script.replace(ISO, (iso) => toIso(toMs(iso) + weeks * 7 * DAY));
}
