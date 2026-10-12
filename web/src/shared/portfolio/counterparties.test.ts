import { describe, expect, it } from "vitest";
import type { Counterparty, Portfolio } from "@/shared/api";
import * as fx from "@/shared/test/fixtures";
import {
	DEFAULT_CSA,
	addExamples,
	assignNettingSet,
	describeCsa,
	nettingSetOf,
	removeCounterparty,
	saveCounterparty,
	tradedUnder,
} from "./counterparties";

const examples = fx.counterpartyExamples() as Counterparty[];
const dealer = examples[0]!;
const book = (): Portfolio =>
	({ ...fx.portfolio(), counterparties: examples }) as Portfolio;

describe("counterparties of a portfolio", () => {
	it("adds the examples once", () => {
		const empty = { ...book(), counterparties: [] };
		const once = { ...empty, ...addExamples(empty, examples) };
		expect(once.counterparties).toHaveLength(examples.length);
		expect(addExamples(once, examples).counterparties).toHaveLength(
			examples.length,
		);
	});

	it("trades a derivative under a netting set, never a listed share", () => {
		const pf = book();
		const set = dealer.netting_sets![0]!;
		const next = {
			...pf,
			...assignNettingSet(
				{ ...pf, ...assignNettingSet(pf, "cac-call", set.id) },
				"EQ:MC.PA",
				set.id,
			),
		};
		expect(tradedUnder(next, set.id).map((i) => i.id)).toEqual(["cac-call"]);
		expect(nettingSetOf(next, set.id)?.counterparty.id).toBe(dealer.id);
	});

	it("keeps a counterparty while a trade is under one of its netting sets", () => {
		const pf = book();
		const set = dealer.netting_sets![0]!;
		const used = { ...pf, ...assignNettingSet(pf, "cac-call", set.id) };
		expect(removeCounterparty(used, dealer.id)).toBeNull();
		const freed = { ...used, ...assignNettingSet(used, "cac-call", null) };
		expect(
			removeCounterparty(freed, dealer.id)?.counterparties?.map((c) => c.id),
		).not.toContain(dealer.id);
	});

	it("replaces a counterparty saved again and leaves the trades alone", () => {
		const pf = book();
		const saved = saveCounterparty(pf, { ...dealer, rating: "BB" });
		expect(saved.counterparties).toHaveLength(examples.length);
		expect(saved.counterparties![0]!.rating).toBe("BB");
		expect(saved.transactions).toBe(pf.transactions);
	});

	it("says what the collateral agreement is", () => {
		expect(describeCsa(null)).toBe("Uncollateralised");
		expect(describeCsa({ ...DEFAULT_CSA, initial_margin: true })).toContain(
			"minimum transfer 500,000, margin period of risk 10 business days, initial margin posted both ways",
		);
	});
});
