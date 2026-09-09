import { describe, expect, it } from "vitest";
import katex from "katex";
import { CATALOG } from "./catalog";
import { PRODUCT_DOCS } from "./docs";

describe("product docs", () => {
	it("every catalog docKey resolves to an entry (no dead keys — WP 12 §3)", () => {
		for (const p of CATALOG) {
			if (!p.docKey) continue;
			expect(
				PRODUCT_DOCS[p.docKey],
				`${p.key} → docKey "${p.docKey}" has no entry`,
			).toBeDefined();
		}
	});

	it("every enabled product has a doc card", () => {
		for (const p of CATALOG.filter((p) => p.enabled)) {
			expect(p.docKey, `${p.key} has no docKey`).toBeTruthy();
		}
	});

	it("every doc carries at least one well-formed reference", () => {
		for (const [key, doc] of Object.entries(PRODUCT_DOCS)) {
			expect(
				doc!.references.length,
				`${key} has no references`,
			).toBeGreaterThan(0);
			for (const r of doc!.references) {
				expect(["paper", "book", "note"], `${key}: bad kind`).toContain(r.kind);
				expect(r.label.length, `${key}: empty label`).toBeGreaterThan(0);
				expect(r.cite.length, `${key}: empty cite`).toBeGreaterThan(10);
				if (r.url)
					expect(r.url, `${key}: ${r.label} url not https`).toMatch(
						/^https:\/\//,
					);
			}
		}
	});

	it("every KaTeX formula renders without throwing (WP 99 acceptance)", () => {
		for (const [key, doc] of Object.entries(PRODUCT_DOCS)) {
			const payoffs = Array.isArray(doc!.payoff) ? doc!.payoff : [doc!.payoff];
			for (const tex of payoffs) {
				expect(() =>
					katex.renderToString(tex, { throwOnError: true, displayMode: true }),
				).not.toThrow();
			}
			// inline math inside prose
			const inline = [
				doc!.summary,
				...doc!.assumptions,
				...doc!.notes,
				...doc!.pricingMethods.map((m) => m.detail),
			]
				.join(" ")
				.match(/\$[^$]+\$/g);
			for (const m of inline ?? []) {
				expect(
					() => katex.renderToString(m.slice(1, -1), { throwOnError: true }),
					`bad inline math in "${key}": ${m}`,
				).not.toThrow();
			}
		}
	});
});
