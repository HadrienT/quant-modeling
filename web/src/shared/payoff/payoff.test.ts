import { describe, expect, it } from "vitest";
import { blackScholes } from "./blackScholes";
import { evaluateStrategy, preset, type Leg } from "./strategy";

const mkt = { spot: 100, rate: 0.05, dividend: 0, vol: 0.2 };

describe("blackScholes", () => {
	it("matches the classic Hull reference (S=K=100, r=5%, σ=20%, T=1)", () => {
		const call = blackScholes({
			spot: 100,
			strike: 100,
			rate: 0.05,
			dividend: 0,
			vol: 0.2,
			maturity: 1,
			isCall: true,
		});
		expect(call.price).toBeCloseTo(10.4506, 2);
		expect(call.delta).toBeCloseTo(0.6368, 3);
	});

	it("respects put–call parity C − P = S·e^{−qT} − K·e^{−rT}", () => {
		const args = {
			spot: 105,
			strike: 100,
			rate: 0.03,
			dividend: 0.01,
			vol: 0.25,
			maturity: 0.75,
		};
		const c = blackScholes({ ...args, isCall: true }).price;
		const p = blackScholes({ ...args, isCall: false }).price;
		const parity =
			args.spot * Math.exp(-args.dividend * args.maturity) -
			args.strike * Math.exp(-args.rate * args.maturity);
		expect(c - p).toBeCloseTo(parity, 4);
	});
});

describe("evaluateStrategy breakevens", () => {
	it("a long straddle breaks even at K ± total premium (analytic)", () => {
		const legs: Leg[] = [
			{
				id: "1",
				kind: "call",
				direction: "long",
				quantity: 1,
				strike: 100,
				maturity: 0.25,
				premium: 4,
			},
			{
				id: "2",
				kind: "put",
				direction: "long",
				quantity: 1,
				strike: 100,
				maturity: 0.25,
				premium: 4,
			},
		];
		const r = evaluateStrategy(legs, mkt, {
			spotRange: [80, 120],
			points: 401,
		});
		expect(r.breakevens).toHaveLength(2);
		expect(r.breakevens[0]).toBeCloseTo(92, 1);
		expect(r.breakevens[1]).toBeCloseTo(108, 1);
	});

	it("a call spread breaks even at Klow + net debit", () => {
		const legs: Leg[] = [
			{
				id: "1",
				kind: "call",
				direction: "long",
				quantity: 1,
				strike: 100,
				maturity: 0.25,
				premium: 5,
			},
			{
				id: "2",
				kind: "call",
				direction: "short",
				quantity: 1,
				strike: 110,
				maturity: 0.25,
				premium: 2,
			},
		];
		const r = evaluateStrategy(legs, mkt, {
			spotRange: [90, 120],
			points: 601,
		});
		expect(r.breakevens[0]).toBeCloseTo(103, 1);
		expect(r.maxGain).not.toBe("unbounded");
		expect(r.maxLoss).not.toBe("unbounded");
	});

	it("a short straddle reports an unbounded loss, not a big number", () => {
		const legs = preset("straddle", 100)!.map((l) => ({
			...l,
			direction: "short" as const,
		}));
		const r = evaluateStrategy(legs, mkt);
		expect(r.maxLoss).toBe("unbounded");
	});
});

describe("value at t → payoff at maturity", () => {
	it("the value-at-t curve converges to the maturity payoff as t → T", () => {
		const legs = preset("call-spread", 100)!;
		const r = evaluateStrategy(legs, mkt, {
			tElapsedFraction: 0.999,
			points: 81,
		});
		for (let i = 0; i < r.spot.length; i++) {
			expect(Math.abs(r.atT[i]! - r.atMaturity[i]!)).toBeLessThan(0.5);
		}
	});
});
