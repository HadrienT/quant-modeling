import { describe, expect, it } from "vitest";
import { splitSegments } from "./segments";

describe("splitSegments", () => {
	it("separates prose from a qms block", () => {
		const out = splitSegments(
			"Here:\n```qms\n2027-09-10\n    pays 1\n```\nDone.",
		);
		expect(out).toEqual([
			{ kind: "text", body: "Here:\n" },
			{ kind: "code", body: "2027-09-10\n    pays 1" },
			{ kind: "text", body: "\nDone." },
		]);
	});

	it("shows an unterminated fence as code while the reply is streaming", () => {
		const out = splitSegments("Ok:\n```qms\n2027-09-10\n    pays ma");
		expect(out.at(-1)).toEqual({
			kind: "code",
			body: "2027-09-10\n    pays ma",
		});
	});

	it("leaves a reply without a fence as one text segment", () => {
		expect(splitSegments("What is the strike?")).toEqual([
			{ kind: "text", body: "What is the strike?" },
		]);
		expect(splitSegments("")).toEqual([]);
	});
});
