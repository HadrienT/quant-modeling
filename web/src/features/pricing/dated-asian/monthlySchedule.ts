/** Monthly fixing dates, one ISO date per line, starting one month after `from`. */
export function monthlySchedule(from: string, months: number): string {
	const base = new Date(from);
	if (Number.isNaN(base.getTime())) return "";
	const out: string[] = [];
	for (let i = 1; i <= months; i += 1) {
		const d = new Date(base);
		d.setMonth(d.getMonth() + i);
		out.push(d.toISOString().slice(0, 10));
	}
	return out.join("\n");
}
