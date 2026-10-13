import { beforeEach, describe, expect, it } from "vitest";
import type { Instrument, Portfolio, PositionMark } from "@/shared/api";
import {
	bookTrade,
	deletePosition,
	deleteTrade,
	equityInstrument,
	netQuantities,
} from "./ledger";
import { aggregateRisk } from "./risk";
import { localRepository, readLocalPortfolios } from "./repository";

function mark(over: Partial<PositionMark>): PositionMark {
	return {
		instrument_id: "EQ:AAPL",
		label: "AAPL",
		kind: "equity",
		currency: "USD",
		quantity: 10,
		average_cost: 100,
		mark: 110,
		market_value: 1100,
		market_value_base: 1000,
		unrealised: 100,
		unrealised_base: 90,
		realised_base: 0,
		fees_base: 0,
		day_pnl_base: 5,
		fx_rate: 0.9,
		inputs: [],
		greeks: { delta: 1 },
		note: null,
		...over,
	};
}

const call: Instrument = {
	id: "c1",
	label: "AAPL call",
	spec: {
		kind: "derivative",
		product: "vanilla",
		params: {},
		underlying: "AAPL",
		expiry: "2027-01-15",
		currency: "USD",
	},
};

describe("aggregateRisk", () => {
	it("groups a stock and its options by underlying — never across names", () => {
		const r = aggregateRisk(
			[
				mark({}),
				mark({
					instrument_id: "c1",
					kind: "derivative",
					quantity: -2,
					greeks: { delta: 0.5 },
				}),
				mark({ instrument_id: "EQ:XOM", label: "XOM", market_value_base: 400 }),
			],
			[equityInstrument("AAPL"), call, equityInstrument("XOM")],
		);
		expect(r.byUnderlying.map((g) => g.underlying)).toEqual(["AAPL", "XOM"]);
		// 10 shares, short 2 calls of delta 0.5: 9 shares' worth
		expect(r.byUnderlying[0]!.delta).toBeCloseTo(9);
	});

	it("keeps rate and FX trades apart: their own risks, no equity greeks", () => {
		const swap: Instrument = {
			id: "irs",
			label: "Receiver swap",
			spec: {
				kind: "rate_fx",
				product: "swap",
				currency: "USD",
				notional: 1e7,
				payer: false,
				start: "2026-10-12",
				maturity: "2031-10-12",
				fixed_rate: 0.0365,
			},
		};
		const r = aggregateRisk(
			[
				mark({}),
				mark({
					instrument_id: "irs",
					label: "Receiver swap",
					kind: "rate_fx",
					currency: "USD",
					quantity: -1,
					// per contract: a payer's, held on the other side
					greeks: { pv01_usd: 4500 },
				}),
			],
			[equityInstrument("AAPL"), swap],
		);
		expect(r.rates).toEqual([
			{
				instrumentId: "irs",
				label: "Receiver swap",
				currency: "USD",
				pv01Usd: -4500,
				pv01Eur: null,
				fxDelta: null,
				normalVega: null,
			},
		]);
		const rates = r.byUnderlying.find((g) => g.underlying === "USD rates")!;
		expect(rates.greeks).toBe(false);
		expect(r.byUnderlying.find((g) => g.underlying === "AAPL")!.greeks).toBe(
			true,
		);
	});

	it("adds the greeks up in money, for the same move of every underlying", () => {
		const r = aggregateRisk(
			[
				// 10 shares at 200 in a currency worth 0.9 of the base.
				mark({ mark: 200, fx_rate: 0.9, greeks: { delta: 1 } }),
				// Short 2 calls on the same share.
				mark({
					instrument_id: "c1",
					kind: "derivative",
					quantity: -2,
					fx_rate: 0.9,
					inputs: [
						{ name: "spot AAPL", status: "observed", value: 200, as_of: null },
					],
					greeks: { delta: 0.5, gamma: 0.02, vega: 40, theta: -7.3, rho: 50 },
				}),
				// A payer swap: 4 500 per basis point, in the base currency.
				mark({
					instrument_id: "irs",
					kind: "rate_fx",
					quantity: 1,
					fx_rate: 1,
					greeks: { pv01_usd: 4500 },
				}),
				// No spot said: counted for the vega, not for the delta.
				mark({
					instrument_id: "x",
					kind: "derivative",
					quantity: 1,
					fx_rate: 1,
					inputs: [],
					greeks: { delta: 0.3, vega: 10 },
				}),
			],
			[equityInstrument("AAPL"), call],
		);
		const t = r.totals;
		expect(t.delta).toBeCloseTo((10 - 2 * 0.5) * 200 * 0.9); // 9 shares' worth
		expect(t.gamma).toBeCloseTo(-2 * 0.02 * 200 * 200 * 0.01 * 0.9);
		expect(t.vega).toBeCloseTo(-2 * 40 * 0.01 * 0.9 + 10 * 0.01);
		expect(t.theta).toBeCloseTo(((-2 * -7.3) / 365) * 0.9);
		expect(t.rates).toBeCloseTo(-2 * 50 * 1e-4 * 0.9 + 4500);
		expect(t.counted).toBe(4);
		expect(t.withoutSpot).toBe(1);
	});

	it("says how many open positions the aggregate covers", () => {
		const r = aggregateRisk(
			[
				mark({}),
				mark({ instrument_id: "c1", market_value_base: null }),
				mark({ quantity: 0, instrument_id: "x" }),
			],
			[equityInstrument("AAPL"), call],
		);
		expect(r.valuedCount).toBe(1);
		expect(r.openCount).toBe(2);
	});
});

describe("ledger", () => {
	const empty = {
		id: "p",
		name: "p",
		owner: "",
		version: 2,
		base_currency: "EUR",
	} as Portfolio;
	const aapl = equityInstrument("aapl");

	it("a sale is a negative quantity; one instrument per ticker", () => {
		let pf = {
			...empty,
			...bookTrade(empty, aapl, {
				instrument_id: aapl.id,
				trade_date: "2026-01-02",
				quantity: 10,
				price: 100,
				fees: 0,
				note: "",
			}),
		};
		pf = {
			...pf,
			...bookTrade(pf, equityInstrument("AAPL"), {
				instrument_id: aapl.id,
				trade_date: "2026-02-02",
				quantity: -15,
				price: 110,
				fees: 1,
				note: "",
			}),
		};
		expect(pf.instruments).toHaveLength(1);
		expect(netQuantities(pf.transactions!).get("EQ:AAPL")).toBe(-5); // now short
	});

	it("deleting the last trade of an instrument drops the instrument", () => {
		const pf = {
			...empty,
			...bookTrade(empty, aapl, {
				instrument_id: aapl.id,
				trade_date: "2026-01-02",
				quantity: 1,
				price: 1,
				fees: 0,
				note: "",
			}),
		};
		const after = deleteTrade(pf, pf.transactions![0]!.id);
		expect(after.transactions).toHaveLength(0);
		expect(after.instruments).toHaveLength(0);
	});
});

describe("deletePosition", () => {
	it("removes every trade of the instrument and the instrument, nothing else", () => {
		const aapl = equityInstrument("AAPL");
		const xom = equityInstrument("XOM");
		const base = {
			id: "p",
			name: "p",
			owner: "",
			version: 2,
			base_currency: "EUR",
		} as Portfolio;
		const t = (id: string, q: number) => ({
			instrument_id: id,
			trade_date: "2026-01-02",
			quantity: q,
			price: 1,
			fees: 0,
			note: "",
		});
		let pf = { ...base, ...bookTrade(base, aapl, t(aapl.id, 5)) };
		pf = { ...pf, ...bookTrade(pf, xom, t(xom.id, 3)) };
		pf = { ...pf, ...bookTrade(pf, aapl, t(aapl.id, -2)) };
		const after = deletePosition(pf, aapl.id);
		expect(after.transactions!.map((x) => x.instrument_id)).toEqual([xom.id]);
		expect(after.instruments!.map((i) => i.id)).toEqual([xom.id]);
	});
});

describe("localRepository", () => {
	beforeEach(() => localStorage.clear());

	it("round-trips a ledger through create → putLedger → get", async () => {
		const pf = await localRepository.create("book");
		const aapl = equityInstrument("AAPL");
		await localRepository.putLedger(
			pf.id,
			bookTrade(pf, aapl, {
				instrument_id: aapl.id,
				trade_date: "2026-01-02",
				quantity: 3,
				price: 5,
				fees: 0,
				note: "",
			}),
		);
		const back = await localRepository.get(pf.id);
		expect(back?.transactions).toHaveLength(1);
		expect(back?.version).toBe(2);
		expect((await localRepository.list())[0]!.n_positions).toBe(1);
		expect(readLocalPortfolios()).toHaveLength(1);
	});

	it("importPortfolio gives the copy a new id", async () => {
		const pf = await localRepository.create("orig");
		const copy = await localRepository.importPortfolio(pf);
		expect(copy.id).not.toBe(pf.id);
	});
});
