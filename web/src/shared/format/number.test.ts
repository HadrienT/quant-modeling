import { describe, expect, it } from "vitest";
import { formatNumber, formatSigned } from "./number";

describe("formatNumber", () => {
	it("renders a dash for missing values", () => {
		expect(formatNumber(null)).toBe("—");
		expect(formatNumber(undefined)).toBe("—");
		expect(formatNumber(NaN)).toBe("—");
	});

	it("prices to 4 decimals", () => {
		expect(formatNumber(12.3, "price")).toBe("12.3000");
	});

	it("shows vol and rate as percentages", () => {
		expect(formatNumber(0.2, "vol")).toBe("20.00%");
		expect(formatNumber(0.045, "rate")).toBe("4.50%");
	});

	it("uses scientific notation for tiny greeks", () => {
		expect(formatNumber(0.00001234, "greek")).toBe("1.23e-5");
	});
});

describe("formatSigned", () => {
	it("prepends an explicit plus for positives", () => {
		expect(formatSigned(3.5)).toBe("+3.50");
	});
	it("uses a real minus sign for negatives", () => {
		expect(formatSigned(-3.5)).toBe("−3.50");
	});
	it("leaves zero unsigned", () => {
		expect(formatSigned(0)).toBe("0.00");
	});
});
