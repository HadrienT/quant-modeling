import { describe, expect, it } from "vitest";
import {
	CATALOG,
	CATALOG_BY_KEY,
	DEFAULT_PRODUCT_KEY,
	resolveProductKey,
} from "./catalog";
import { COVERED_BY_PRICERS, SCRIPTED_PRODUCTS, scriptedKey } from "./scripted";
import { fieldsFromSchema } from "./zodFields";

describe("scripted catalog entries", () => {
	it("offers every product of the script library on the pricing page, once", () => {
		expect(SCRIPTED_PRODUCTS.length).toBeGreaterThanOrEqual(40);
		for (const p of SCRIPTED_PRODUCTS) {
			if (COVERED_BY_PRICERS.has(p.slug)) {
				// A pricer of the catalog is this product: no second entry, and
				// an old link to the script opens the pricer.
				expect(CATALOG_BY_KEY.has(scriptedKey(p.slug)), p.slug).toBe(false);
				const opened = resolveProductKey(scriptedKey(p.slug));
				expect(CATALOG_BY_KEY.get(opened!)?.scripted, p.slug).toBeUndefined();
				continue;
			}
			const d = CATALOG_BY_KEY.get(scriptedKey(p.slug))!;
			expect(d.enabled && d.endpoint).toBe("/price/scripted-product");
			expect(d.schema.safeParse(d.defaults).success, p.slug).toBe(true);
		}
		for (const slug of COVERED_BY_PRICERS)
			expect(
				SCRIPTED_PRODUCTS.some((p) => p.slug === slug),
				slug,
			).toBe(true);
	});

	it("lists no product twice and offers only Analytic and Monte-Carlo", () => {
		const labels = CATALOG.map((p) => p.label.toLowerCase());
		expect(new Set(labels).size).toBe(labels.length);
		for (const p of CATALOG.filter((d) => d.enabled))
			for (const e of p.engines)
				expect(["analytic", "mc"], `${p.key}: ${e.key}`).toContain(e.key);
	});

	it("sends terms as fractions and one ticker per underlying", () => {
		const d = CATALOG_BY_KEY.get("script-worst-of-autocall")!;
		const body = d.toRequest(d.defaults, "mc") as Record<string, unknown>;
		expect(body).toMatchObject({
			product: "worst-of-autocall",
			model: "auto",
			underlyings: [
				{ ticker: "AAPL" },
				{ ticker: "MSFT" },
				{ ticker: "GOOGL" },
			],
			rate: 0.04,
		});
		expect((body.terms as Record<string, number>).coupon).toBeCloseTo(0.025);
	});

	it("labels the terms the way the script declares them", () => {
		const d = CATALOG_BY_KEY.get("script-barrier-reverse-convertible")!;
		const barrier = fieldsFromSchema(d.schema, d.fields).find(
			(f) => f.name === "barrier",
		)!;
		expect(barrier).toMatchObject({
			label: "Knock-in barrier, % of the initial level",
			unit: "%",
			group: "contract",
		});
	});

	it("lists every product by family, hand-written pricers and scripts mixed", () => {
		const cats = CATALOG.map((p) => p.category);
		// One run per family, in order: no family is split in two.
		const runs = cats.filter((c, i) => c !== cats[i - 1]);
		expect(runs).toEqual([...new Set(cats)]);
		expect(runs.slice(0, 8)).toEqual([
			"vanilla",
			"barrier",
			"path",
			"choice",
			"cliquet",
			"note",
			"volatility",
			"multi",
		]);
		// Within a family the hand-written pricers come first.
		for (const fam of runs) {
			const kinds = CATALOG.filter((p) => p.category === fam).map(
				(p) => !!p.scripted,
			);
			expect(kinds).toEqual([...kinds].sort((a, b) => +a - +b));
		}
		expect(CATALOG_BY_KEY.get(DEFAULT_PRODUCT_KEY)?.enabled).toBe(true);
	});

	it("opens the script of a product that used to be greyed out", () => {
		for (const [former, script] of [
			["autocall", "script-athena-autocall"],
			["mountain", "script-himalaya"],
			["variance-swap", "script-variance-swap"],
			["volatility-swap", "script-volatility-swap"],
			["dispersion-swap", "script-dispersion-swap"],
			["double-barrier", "script-double-barrier-call"],
			["chooser", "script-chooser"],
			["compound", "script-compound"],
		]) {
			const key = resolveProductKey(former)!;
			expect(key).toBe(script);
			expect(CATALOG_BY_KEY.get(key)?.enabled, former).toBe(true);
			// Its reference sheet followed it.
			expect(CATALOG_BY_KEY.get(key)?.docKey, former).toBe(
				former === "double-barrier" ? "double-barrier" : former,
			);
		}
		expect(resolveProductKey("vanilla")).toBe("vanilla");
	});

	it("leaves only the asset classes without market data unpriced", () => {
		expect(
			CATALOG.filter((p) => !p.enabled)
				.map((p) => p.category)
				.every((c) => c === "fx" || c === "commodity"),
		).toBe(true);
	});

	it("keys never collide with the hand-written catalog", () => {
		expect(new Set(CATALOG.map((p) => p.key)).size).toBe(CATALOG.length);
	});
});
