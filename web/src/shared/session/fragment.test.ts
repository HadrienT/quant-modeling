import { describe, expect, it } from "vitest";
import { parseAuthFragment } from "./fragment";

describe("parseAuthFragment", () => {
	it("extracts the token from a successful Google callback", () => {
		expect(parseAuthFragment("#qm_auth=abc.def.ghi")).toEqual({
			token: "abc.def.ghi",
		});
	});

	it("extracts a decoded error reason", () => {
		expect(parseAuthFragment("#qm_auth_error=Invalid%20OAuth%20state")).toEqual(
			{
				error: "Invalid OAuth state",
			},
		);
	});

	it("ignores unrelated fragments, so anchors keep working", () => {
		expect(parseAuthFragment("")).toBeNull();
		expect(parseAuthFragment("#section-2")).toBeNull();
	});
});
