import { describe, expect, it } from "vitest";
import { SCRIPTED_PRODUCTS } from "./scripted";
import {
	CITE,
	citedRefs,
	implementationOf,
	measuredOf,
} from "./implementation";
import { METHOD_REFS } from "./methodRefs";

describe("How it is implemented", () => {
	it("measures every product of the library", () => {
		for (const p of SCRIPTED_PRODUCTS)
			expect(measuredOf(p.slug), p.slug).toBeDefined();
	});

	it("cites only references that exist, each numbered once", () => {
		for (const p of SCRIPTED_PRODUCTS) {
			const impl = implementationOf(p);
			if (!impl || "notice" in impl) continue;
			const keys = impl.refs.map((r) => r.key);
			expect(new Set(keys).size, p.slug).toBe(keys.length);
			for (const { key, ref } of impl.refs)
				expect(ref, `${p.slug}: ${key}`).toBeDefined();
			for (const s of impl.sections)
				for (const l of s.lines) {
					expect(l.text.trim(), `${p.slug} ${l.label}`).not.toBe("");
					// a citation outside the [@key] syntax would not be numbered
					expect(l.text.replace(CITE, ""), p.slug).not.toMatch(/\[@|@\w+\]/);
				}
		}
	});

	it("gives every reference a label, a citation and a DOI link if any", () => {
		for (const [key, r] of Object.entries(METHOD_REFS)) {
			expect(r.label, key).toMatch(/\(\d{4}[a-c]?\)$/);
			expect(r.cite.length, key).toBeGreaterThan(20);
			if ("url" in r) expect(r.url, key).toMatch(/^https:\/\/doi\.org\/10\./);
		}
	});

	it("numbers references by first citation", () => {
		const refs = citedRefs([
			{
				title: "t",
				lines: [
					{ label: "a", text: "x [@sobol; @joeKuo] y [@sobol]" },
					{ label: "b", text: "z [@lsm]" },
				],
			},
		]);
		expect(refs.map((r) => r.key)).toEqual(["sobol", "joeKuo", "lsm"]);
	});

	it("is written in English", () => {
		for (const p of SCRIPTED_PRODUCTS) {
			const impl = implementationOf(p);
			if (!impl || "notice" in impl) continue;
			for (const s of impl.sections)
				for (const l of s.lines) expect(l.text, p.slug).not.toMatch(/[éèêàùç]/);
		}
	});
});
