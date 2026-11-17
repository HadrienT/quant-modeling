import { describe, expect, it } from "vitest";
import type { Instrument, PositionMark } from "@/shared/api";
import { decodeParams } from "@/shared/products";
import { workbenchFromPosition } from "./workbenchFromPosition";

const TODAY = Date.parse("2026-11-17T00:00:00Z");

const instrument = {
	id: "i1",
	label: "SPY European option K=500 2027-05-18",
	spec: {
		kind: "derivative",
		product: "vanilla",
		underlying: "SPY",
		expiry: "2027-05-18",
		currency: "USD",
		params: {
			spot: 480,
			strike: 500,
			maturity: 1,
			rate: 0.04,
			dividend: 0,
			vol: 0.2,
			is_call: true,
			engine: "analytic",
			n_paths: 200000,
			seed: 1,
			tree_steps: 200,
		},
	},
} as unknown as Instrument;

function position(inputs: { name: string; value: number }[]): PositionMark {
	return {
		instrument_id: "i1",
		kind: "derivative",
		inputs: inputs.map((i) => ({ ...i, as_of: "2026-11-16", status: "ok" })),
	} as unknown as PositionMark;
}

/** A position opens the workbench on its terms and today's market (WP 28, U2). */
describe("workbench link of a position", () => {
	it("keeps the contract terms and takes the latest mark's market inputs", () => {
		const link = workbenchFromPosition(
			position([
				{ name: "spot", value: 512.3 },
				{ name: "rate", value: 0.0431 },
				{ name: "vol", value: 0.187 },
			]),
			instrument,
			TODAY,
		);
		expect(link?.product).toBe("vanilla");
		const values = decodeParams<Record<string, unknown>>(link!.p, {});
		expect(values).toMatchObject({
			strike: 500,
			is_call: true,
			spot: 512.3,
			rate: 4.31,
			vol: 18.7,
			dividend: 0,
		});
		// 182 days left, in years
		expect(values.maturity).toBeCloseTo(182 / 365.25, 4);
	});

	it("offers nothing for an expired position", () => {
		expect(
			workbenchFromPosition(position([]), instrument, Date.parse("2027-06-01")),
		).toBeNull();
	});

	it("offers nothing for an equity", () => {
		const equity = {
			instrument_id: "i2",
			kind: "equity",
			inputs: [],
		} as unknown as PositionMark;
		expect(workbenchFromPosition(equity, undefined, TODAY)).toBeNull();
	});
});
