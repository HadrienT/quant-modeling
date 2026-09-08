import { beforeEach, describe, expect, it } from "vitest";
import type { Position } from "@/shared/api";
import { aggregateRisk } from "./risk";
import { localRepository, readLocalPortfolios } from "./repository";

function pos(over: Partial<Position>): Position {
	return {
		id: over.id ?? Math.random().toString(36).slice(2),
		label: "AAPL call",
		product_type: "vanilla" as never,
		category: "vanilla" as never,
		direction: "long",
		quantity: 10,
		entry_price: 5,
		parameters: {},
		result: null,
		...over,
	} as Position;
}

describe("aggregateRisk", () => {
	it("groups greeks per underlying — never sums across names", () => {
		const r = aggregateRisk([
			pos({
				label: "AAPL call",
				result: {
					npv: 100,
					unit_price: 10,
					greeks: { delta: 0.5 },
					engine: "analytic",
					diagnostics: "",
					mc_std_error: 0,
					priced_at: "2026-02-01T10:00:00Z",
				},
			}),
			pos({
				label: "XOM put",
				result: {
					npv: 40,
					unit_price: 4,
					greeks: { delta: -0.3 },
					engine: "analytic",
					diagnostics: "",
					mc_std_error: 0,
					priced_at: "2026-01-15T10:00:00Z",
				},
			}),
		]);
		expect(r.byUnderlying).toHaveLength(2);
		expect(r.totalNpv).toBe(140);
		expect(r.oldestPricedAt).toBe("2026-01-15T10:00:00Z");
	});

	it("reports how many positions the aggregate actually covers", () => {
		const r = aggregateRisk([
			pos({ result: null }),
			pos({
				result: {
					npv: 10,
					unit_price: 1,
					greeks: {},
					engine: "mc",
					diagnostics: "",
					mc_std_error: 0.5,
				},
			}),
		]);
		expect(r.pricedCount).toBe(1);
		expect(r.totalCount).toBe(2);
	});
});

describe("localRepository", () => {
	beforeEach(() => localStorage.clear());

	it("round-trips a portfolio through create → putPositions → get", async () => {
		const pf = await localRepository.create("book");
		await localRepository.putPositions(pf.id, [pos({ label: "x" })]);
		const back = await localRepository.get(pf.id);
		expect(back?.positions).toHaveLength(1);
		expect(readLocalPortfolios()).toHaveLength(1);
	});

	it("importPortfolio gives the copy a new id", async () => {
		const pf = await localRepository.create("orig");
		const copy = await localRepository.importPortfolio({
			...pf,
			positions: [],
		});
		expect(copy.id).not.toBe(pf.id);
	});
});
