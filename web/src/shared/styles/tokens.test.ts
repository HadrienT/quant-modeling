import { describe, expect, it } from "vitest";
import { divergentColor, sequentialColor, type ThemeTokens } from "./tokens";

const tokens = {
	divergent: ["#2a78d6", "#d03b3b"] as [string, string],
	diverneutral: "#7d8899",
	sequential: ["#cde2fb", "#7fb2ee", "#3987e5", "#1f5bb0", "#0d366b"],
} as ThemeTokens;

describe("divergentColor", () => {
	it("returns the neutral grey at zero", () => {
		expect(divergentColor(0, tokens)).toBe("rgb(125 136 153)");
	});
	it("moves toward blue for negatives and red for positives", () => {
		expect(divergentColor(-1, tokens)).toBe("rgb(42 120 214)");
		expect(divergentColor(1, tokens)).toBe("rgb(208 59 59)");
	});
	it("clamps out-of-range input", () => {
		expect(divergentColor(-5, tokens)).toBe(divergentColor(-1, tokens));
	});
});

describe("sequentialColor", () => {
	it("hits the ramp endpoints", () => {
		expect(sequentialColor(0, tokens)).toBe("rgb(205 226 251)");
		expect(sequentialColor(1, tokens)).toBe("rgb(13 54 107)");
	});
	it("is monotone in lightness (single hue, no rainbow)", () => {
		const lum = (rgb: string) => {
			const [r, g, b] = rgb.match(/\d+/g)!.map(Number);
			return 0.2126 * r! + 0.7152 * g! + 0.0722 * b!;
		};
		const samples = [0, 0.25, 0.5, 0.75, 1].map((t) =>
			lum(sequentialColor(t, tokens)),
		);
		for (let i = 1; i < samples.length; i++) {
			expect(samples[i]!).toBeLessThan(samples[i - 1]!);
		}
	});
});
