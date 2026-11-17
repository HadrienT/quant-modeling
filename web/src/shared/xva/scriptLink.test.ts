import { describe, expect, it } from "vitest";
import {
	MAX_SCRIPT,
	decodeScript,
	encodeScript,
	isRateScript,
} from "./scriptLink";

describe("a script carried in the /xva URL", () => {
	it("round-trips any text, accents and symbols included, URL-safe", () => {
		const script = "x = df(T) * max(swap(T, 5y) − K, 0) // σ, €, +/=\n";
		const encoded = encodeScript(script);
		expect(encoded).toMatch(/^[A-Za-z0-9_-]+$/);
		expect(decodeScript(encoded)).toBe(script);
	});

	it("reads a malformed or missing parameter as no script", () => {
		expect(decodeScript(undefined)).toBeNull();
		expect(decodeScript("%%%")).toBeNull();
	});

	it("offers only rate scripts the exposure engine can value", () => {
		expect(isRateScript("pay df(T) * 1", 0)).toBe(true);
		expect(isRateScript("pay max(S - K, 0)", 1)).toBe(false);
		expect(isRateScript("pay df(T) * S", 1)).toBe(false);
		expect(isRateScript(`pay df(T)${" ".repeat(MAX_SCRIPT)}`, 0)).toBe(false);
	});
});
