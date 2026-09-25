import { describe, expect, it } from "vitest";
import { classify } from "./riskProfile";

describe("classify (same rules as api/app/risk_profile.py)", () => {
	it("reads the holder's side from the sign of the change", () => {
		expect(classify(0.5, 0.1, 10)).toBe("long");
		expect(classify(-0.5, 0.1, 10)).toBe("short");
	});

	it("never calls a sign inside two standard errors", () => {
		expect(classify(0.19, 0.1, 10)).toBe("not significant");
		expect(classify(-0.2, 0.1, 10)).toBe("not significant");
	});

	it("calls an exact but tiny change negligible", () => {
		expect(classify(0.0005, 0, 1000)).toBe("negligible");
		expect(classify(0.5, 0, 1000)).toBe("long");
	});
});
