import { describe, expect, it } from "vitest";
import {
	CATALOG,
	CATALOG_BY_KEY,
	DEFAULT_PRODUCT_KEY,
	resolveProductKey,
} from "./catalog";
import { ANALYTIC_SCRIPTED, SCRIPTED_PRODUCTS, scriptedKey } from "./scripted";
import { fieldsFromSchema } from "./zodFields";

describe("scripted catalog entries", () => {
	it("offers every product of the script library on the pricing page", () => {
		expect(SCRIPTED_PRODUCTS.length).toBeGreaterThanOrEqual(40);
		for (const p of SCRIPTED_PRODUCTS) {
			const d = CATALOG_BY_KEY.get(scriptedKey(p.slug))!;
			expect(CATALOG).toContain(d);
			expect(d.endpoint).toBe("/price/scripted-product");
			expect(d.enabled || !!d.disabledReason, p.slug).toBe(true);
			expect(d.schema.safeParse(d.defaults).success, p.slug).toBe(true);
		}
	});

	it("lists scripts, not the hand-written pricers, once each, Analytic and Monte-Carlo only", () => {
		const labels = CATALOG.map((p) => p.label.toLowerCase());
		expect(new Set(labels).size).toBe(labels.length);
		for (const key of [
			"vanilla",
			"american",
			"asian",
			"barrier",
			"digital",
			"lookback",
			"basket",
			"rainbow",
			"quanto",
			"future",
			"fixed-rate-bond",
			"zero-coupon-bond",
		]) {
			expect(
				CATALOG.some((p) => p.key === key),
				key,
			).toBe(false);
			// an old link opens the script that replaced it
			expect(resolveProductKey(key)?.startsWith("script-"), key).toBe(true);
			expect(CATALOG_BY_KEY.has(resolveProductKey(key)!), key).toBe(true);
		}
		for (const p of CATALOG.filter((d) => d.enabled))
			for (const e of p.engines)
				expect(["analytic", "mc"], `${p.key}: ${e.key}`).toContain(e.key);
		for (const slug of ANALYTIC_SCRIPTED) {
			const d = CATALOG_BY_KEY.get(scriptedKey(slug))!;
			expect(d.engines[0]!.key).toBe("analytic");
			expect(d.toRequest(d.defaults, "analytic")).toMatchObject({
				engine: "analytic",
			});
			expect(d.toRequest(d.defaults, "mc")).not.toHaveProperty("engine");
		}
	});

	it("prices a rates product on a government curve, with no ticker and no model", () => {
		const d = CATALOG_BY_KEY.get("script-fixed-rate-bond")!;
		expect(d.category).toBe("fixed-income");
		expect(d.engines.map((e) => e.key)).toEqual(["analytic", "mc"]);
		const body = d.toRequest(
			{ ...d.defaults, currency: "EUR" },
			"analytic",
		) as Record<string, unknown>;
		expect(body).toMatchObject({
			product: "fixed-rate-bond",
			hull_white: { currency: "EUR" },
			engine: "analytic",
		});
		expect(body).not.toHaveProperty("ticker");
		expect(body).not.toHaveProperty("model");
		expect((body.terms as Record<string, number>).coupon).toBeCloseTo(0.04);
	});

	it("prices a quanto on one underlying in its own currency, paid in the declared one", () => {
		const d = CATALOG_BY_KEY.get("script-quanto-call")!;
		expect(d.toRequest(d.defaults, "mc")).toMatchObject({
			underlyings: [{ ticker: "SAP.DE" }],
			currency: "USD",
		});
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
		expect(resolveProductKey("script-chooser")).toBe("script-chooser");
	});

	it("leaves only what the database cannot price unpriced", () => {
		// FX and commodities, and the quanto, whose foreign underlying has no
		// stored dividend yield yet
		expect(
			CATALOG.filter((p) => !p.enabled).every(
				(p) =>
					p.category === "fx" ||
					p.category === "commodity" ||
					p.key === "script-quanto-call",
			),
		).toBe(true);
	});

	it("keys never collide with the hand-written catalog", () => {
		expect(new Set(CATALOG.map((p) => p.key)).size).toBe(CATALOG.length);
	});
});
