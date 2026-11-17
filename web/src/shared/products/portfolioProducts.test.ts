import { describe, expect, it } from "vitest";
import { CATALOG_BY_KEY } from "./catalog";
import {
	PORTFOLIO_PRODUCTS,
	catalogKeyOf,
	formValuesFromRequest,
} from "./portfolioProducts";

/**
 * The inverse of `toRequest` that lets a position open the workbench
 * (WP 28, lot U2), as a property: for every product a portfolio can hold,
 * a request built from changed form values comes back to those values.
 */
describe("form values from a request", () => {
	for (const key of Object.keys(PORTFOLIO_PRODUCTS)) {
		it(`round-trips ${key}`, () => {
			const d = CATALOG_BY_KEY.get(key)!;
			const engine = d.engines[0]!.key;
			// Move every numeric field off its default, percent fields included.
			const changed = Object.fromEntries(
				Object.entries(d.defaults).map(([k, v]) => [
					k,
					typeof v === "number" ? +(v * 1.1 + 0.3).toFixed(6) : v,
				]),
			);
			const request = d.toRequest(changed, engine) as Record<string, unknown>;
			const values = formValuesFromRequest(d, request);
			expect(values).not.toBeNull();
			for (const [k, v] of Object.entries(changed))
				if (typeof v === "number" && k in request)
					expect(values![k]).toBeCloseTo(v, 9);
		});
	}

	it("reads percent fields off toRequest: a 25% vol is 0.25 in the request", () => {
		const d = CATALOG_BY_KEY.get("vanilla")!;
		const values = formValuesFromRequest(d, {
			spot: 512,
			strike: 500,
			maturity: 0.5,
			rate: 0.043,
			dividend: 0.012,
			vol: 0.25,
			is_call: false,
		});
		expect(values).toMatchObject({
			spot: 512,
			strike: 500,
			maturity: 0.5,
			rate: 4.3,
			dividend: 1.2,
			vol: 25,
			is_call: false,
		});
	});

	it("maps a registry id back to its catalog key", () => {
		expect(catalogKeyOf("american_vanilla")).toBe("american");
		expect(catalogKeyOf("swap")).toBeUndefined();
	});
});
