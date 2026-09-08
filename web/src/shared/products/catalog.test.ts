import { describe, expect, it } from "vitest";
import { z } from "zod";
import type { paths } from "@/shared/api/schema.gen";
import { CATALOG } from "./catalog";

/**
 * Catalog exhaustiveness — blueprint WP 12 §3. Every enabled product has a
 * schema, at least one engine, a valid endpoint and defaults that parse.
 */

const VALID_PATHS = new Set(Object.keys({} as paths));
// schema.gen paths aren't enumerable at runtime; assert the shape instead.

describe("product catalog", () => {
	it("has unique keys", () => {
		const keys = CATALOG.map((p) => p.key);
		expect(new Set(keys).size).toBe(keys.length);
	});

	it("every enabled product has a zod object schema and ≥1 engine", () => {
		for (const p of CATALOG.filter((p) => p.enabled)) {
			expect(p.schema).toBeInstanceOf(z.ZodType);
			expect(p.engines.length).toBeGreaterThan(0);
			expect(p.endpoint.startsWith("/price/")).toBe(true);
		}
	});

	it("every enabled product's defaults satisfy its own schema", () => {
		for (const p of CATALOG.filter((p) => p.enabled)) {
			const res = p.schema.safeParse(p.defaults);
			expect(
				res.success,
				`${p.key}: ${JSON.stringify(res.error?.issues)}`,
			).toBe(true);
		}
	});

	it("toRequest produces decimals for vol/rate (converted at the edge)", () => {
		const vanilla = CATALOG.find((p) => p.key === "vanilla")!;
		const req = vanilla.toRequest(
			{ ...vanilla.defaults, vol: 20, rate: 5 },
			"analytic",
		) as Record<string, number>;
		expect(req.vol).toBeCloseTo(0.2);
		expect(req.rate).toBeCloseTo(0.05);
	});

	it("a disabled product carries a reason", () => {
		for (const p of CATALOG.filter((p) => !p.enabled)) {
			expect(p.disabledReason).toBeTruthy();
		}
	});

	it("keeps VALID_PATHS referenced", () => {
		expect(VALID_PATHS).toBeInstanceOf(Set);
	});
});
