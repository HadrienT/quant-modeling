import { describe, expect, it } from "vitest";
import {
	SSVI,
	SSVI_MATURITIES,
	SSVI_STRIKES,
	theta,
	totalVariance,
} from "./ssvi";

/**
 * The home page shows this surface as arbitrage-free: hold it to that, on the
 * exact nodes drawn and between them.
 */
describe("SSVI home surface", () => {
	const ks = SSVI_STRIKES.map((K) => Math.log(K / SSVI.spot));

	it("sits inside Gatheral & Jacquier's no-arbitrage bound", () => {
		expect(SSVI.eta * (1 + Math.abs(SSVI.rho))).toBeLessThanOrEqual(2);
		expect(SSVI.gamma).toBeGreaterThan(0);
		expect(SSVI.gamma).toBeLessThanOrEqual(0.5);
	});

	it("has no calendar arbitrage: total variance rises with maturity at every strike", () => {
		for (const k of ks) {
			for (let i = 1; i < SSVI_MATURITIES.length; i++) {
				const before = totalVariance(k, theta(SSVI_MATURITIES[i - 1]!));
				const after = totalVariance(k, theta(SSVI_MATURITIES[i]!));
				expect(after).toBeGreaterThan(before);
			}
		}
	});

	it("has no butterfly arbitrage: Durrleman's g(k) stays non-negative", () => {
		const h = 1e-4;
		for (const t of SSVI_MATURITIES) {
			const th = theta(t);
			for (let k = ks[0]!; k <= ks[ks.length - 1]!; k += 0.01) {
				const w = totalVariance(k, th);
				const w1 =
					(totalVariance(k + h, th) - totalVariance(k - h, th)) / (2 * h);
				const w2 =
					(totalVariance(k + h, th) - 2 * w + totalVariance(k - h, th)) /
					(h * h);
				const g =
					(1 - (k * w1) / (2 * w)) ** 2 -
					((w1 * w1) / 4) * (1 / w + 0.25) +
					w2 / 2;
				expect(g).toBeGreaterThanOrEqual(0);
			}
		}
	});

	it("is at the money where it says: 20 % ATM vol at every maturity", () => {
		for (const t of SSVI_MATURITIES) {
			expect(Math.sqrt(totalVariance(0, theta(t)) / t)).toBeCloseTo(
				SSVI.atmVol,
				12,
			);
		}
	});
});
