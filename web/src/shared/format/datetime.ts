/** Date / relative-time formatting — blueprint WP 01 §6 (Freshness primitive). */

const DATE = new Intl.DateTimeFormat("en-GB", {
	day: "2-digit",
	month: "2-digit",
	year: "numeric",
});

const DATETIME = new Intl.DateTimeFormat("en-GB", {
	day: "2-digit",
	month: "2-digit",
	year: "numeric",
	hour: "2-digit",
	minute: "2-digit",
});

const RELATIVE = new Intl.RelativeTimeFormat("en", { numeric: "auto" });

export function formatDate(value: string | number | Date | null | undefined) {
	if (value == null) return "—";
	const d = value instanceof Date ? value : new Date(value);
	if (Number.isNaN(d.getTime())) return "—";
	return DATE.format(d);
}

export function formatDateTime(
	value: string | number | Date | null | undefined,
) {
	if (value == null) return "—";
	const d = value instanceof Date ? value : new Date(value);
	if (Number.isNaN(d.getTime())) return "—";
	return DATETIME.format(d);
}

const STEPS: [Intl.RelativeTimeFormatUnit, number][] = [
	["year", 365 * 24 * 3600],
	["month", 30 * 24 * 3600],
	["day", 24 * 3600],
	["hour", 3600],
	["minute", 60],
	["second", 1],
];

/** "calculated 4 min ago" style phrasing. `now` is injectable for tests. */
export function formatRelative(
	value: string | number | Date | null | undefined,
	now: number = Date.now(),
): string {
	if (value == null) return "—";
	const d = value instanceof Date ? value : new Date(value);
	if (Number.isNaN(d.getTime())) return "—";
	const deltaSec = (d.getTime() - now) / 1000;
	const abs = Math.abs(deltaSec);
	for (const [unit, secs] of STEPS) {
		if (abs >= secs || unit === "second") {
			return RELATIVE.format(Math.round(deltaSec / secs), unit);
		}
	}
	return RELATIVE.format(0, "second");
}

export function ageSeconds(
	value: string | number | Date | null | undefined,
	now: number = Date.now(),
): number | null {
	if (value == null) return null;
	const d = value instanceof Date ? value : new Date(value);
	if (Number.isNaN(d.getTime())) return null;
	return (now - d.getTime()) / 1000;
}
