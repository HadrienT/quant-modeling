import { describe, expect, it } from "vitest";
import { alignToValuationDate } from "./alignDates";
import { CATEGORY_ORDER, SCRIPT_LIBRARY, parseProduct } from "./library";
import { countUnderlyings, underlyingsRequest } from "./underlyings";

const day = (iso: string) => Date.parse(`${iso}T00:00:00Z`);

describe("the product library", () => {
	it("loads every product with what the page shows about it", () => {
		expect(SCRIPT_LIBRARY.length).toBeGreaterThanOrEqual(40);
		for (const p of SCRIPT_LIBRARY) {
			expect(CATEGORY_ORDER).toContain(p.category);
			expect(p.title).not.toBe(p.slug);
			expect(p.summary.length).toBeGreaterThan(20);
			expect(p.sources.length).toBeGreaterThan(0);
			expect(p.script).not.toMatch(/^# (title|category|source):/m);
			// The count the page infers from the script is the declared one.
			expect(countUnderlyings(p.script)).toBe(p.underlyings);
		}
	});

	it("keeps the explanatory comments and drops the header", () => {
		const p = parseProduct(
			"x",
			"# title: X\n# category: Cliquets\n# underlyings: 2\n# source: A\n# source: B\n# summary: S\n#\n# Terms.\n2027-01-04\n    pays 1\n",
		);
		expect(p).toMatchObject({
			title: "X",
			underlyings: 2,
			sources: ["A", "B"],
		});
		expect(p.script.startsWith("# Terms.\n2027-01-04")).toBe(true);
	});
});

describe("alignToValuationDate", () => {
	const script =
		"2027-01-04\n    s0 = spot()\nschedule(2027-01-05, 2027-12-30, 1D, US, F)\n2028-01-04\n";

	it("starts the product a week or so after the valuation date", () => {
		const out = alignToValuationDate(script, "2026-09-25");
		const first = day(out.match(/\d{4}-\d{2}-\d{2}/)![0]);
		const lead = (first - day("2026-09-25")) / 86_400_000;
		expect(lead).toBeGreaterThanOrEqual(7);
		expect(lead).toBeLessThan(14);
	});

	it("moves every date by whole weeks, keeping weekdays and spacing", () => {
		const before = script.match(/\d{4}-\d{2}-\d{2}/g)!.map(day);
		const after = alignToValuationDate(script, "2031-06-10")
			.match(/\d{4}-\d{2}-\d{2}/g)!
			.map(day);
		const shift = after[0]! - before[0]!;
		expect(shift % (7 * 86_400_000)).toBe(0);
		after.forEach((d, i) => expect(d - before[i]!).toBe(shift));
	});

	it("leaves a script without dates alone", () => {
		expect(alignToValuationDate("x = 1", "2026-09-25")).toBe("x = 1");
	});
});

describe("underlyings", () => {
	it("counts spot(i) indices, not the ones in comments", () => {
		expect(countUnderlyings("pays spot()")).toBe(1);
		expect(countUnderlyings("# spot(5)\npays spot(0) - spot(2)")).toBe(3);
	});

	it("builds typed inputs with one correlation for every pair", () => {
		const u = {
			tickers: "AAPL MSFT",
			assets: [
				{ spot: "100", volPct: "25", divPct: "1" },
				{ spot: "50", volPct: "30", divPct: "0" },
			],
			corrPct: "40",
		} as unknown as Parameters<typeof underlyingsRequest>[2];
		expect(underlyingsRequest(2, false, u)).toEqual({
			underlyings: [
				{ spot: 100, vol: 0.25, dividend: 0.01 },
				{ spot: 50, vol: 0.3, dividend: 0 },
			],
			correlation: [
				[1, 0.4],
				[0.4, 1],
			],
		});
		expect(underlyingsRequest(2, true, u)).toEqual({
			underlyings: [{ ticker: "AAPL" }, { ticker: "MSFT" }],
		});
	});
});
