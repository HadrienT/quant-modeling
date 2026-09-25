import { describe, expect, it } from "vitest";
import { CATALOG, CATALOG_BY_KEY } from "./catalog";
import { SCRIPTED_PRODUCTS, scriptedKey } from "./scripted";
import { fieldsFromSchema } from "./zodFields";

describe("scripted catalog entries", () => {
	it("offers every product of the script library on the pricing page", () => {
		expect(SCRIPTED_PRODUCTS.length).toBeGreaterThanOrEqual(40);
		for (const p of SCRIPTED_PRODUCTS) {
			const d = CATALOG_BY_KEY.get(scriptedKey(p.slug))!;
			expect(d.enabled && d.endpoint).toBe("/price/scripted-product");
			expect(d.schema.safeParse(d.defaults).success, p.slug).toBe(true);
		}
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

	it("keys never collide with the hand-written catalog", () => {
		expect(new Set(CATALOG.map((p) => p.key)).size).toBe(CATALOG.length);
	});
});
