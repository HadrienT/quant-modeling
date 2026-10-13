import { describe, expect, it } from "vitest";
import { normalizeMath } from "./math";

describe("normalizeMath", () => {
	it("keeps a well-formed inline formula", () => {
		expect(normalizeMath("pays $\\max(S_T - K, 0)$ at expiry")).toBe(
			"pays $\\max(S_T - K, 0)$ at expiry",
		);
	});

	it("rewrites \\( \\) and \\[ \\] into dollar delimiters", () => {
		expect(normalizeMath("with \\( S_T \\) the spot")).toBe(
			"with $S_T$ the spot",
		);
		expect(normalizeMath("so\n\\[ A = \\frac{1}{n} \\]\nthen")).toBe(
			"so\n\n\n$$\nA = \\frac{1}{n}\n$$\n\n\nthen",
		);
	});

	it("puts a $$ formula on its own lines, where it is a display block", () => {
		expect(normalizeMath("so $$x^2$$ then")).toBe(
			"so \n\n$$\nx^2\n$$\n\n then",
		);
	});

	it("escapes dollars that are prices, not formulas", () => {
		expect(normalizeMath("strike $100 and barrier $130")).toBe(
			"strike \\$100 and barrier \\$130",
		);
		expect(normalizeMath("between $100-$130")).toBe("between \\$100-\\$130");
		expect(normalizeMath("costs $5, with $x$ the spot")).toBe(
			"costs \\$5, with $x$ the spot",
		);
	});

	it("escapes a formula that is still being streamed", () => {
		expect(normalizeMath("so $$ \\frac{1}{")).toBe("so \\$\\$ \\frac{1}{");
		expect(normalizeMath("with $S_")).toBe("with \\$S_");
	});

	it("leaves code spans alone", () => {
		expect(normalizeMath("use `$x$` and ``\\(a\\)``")).toBe(
			"use `$x$` and ``\\(a\\)``",
		);
	});
});
