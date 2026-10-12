import type {
	Counterparty,
	CsaTerms,
	Instrument,
	NettingSet,
	Portfolio,
} from "@/shared/api";
import type { Ledger } from "./repository";

/**
 * Counterparties and netting sets of a portfolio, front side: who its
 * over-the-counter derivatives face and under which agreement. The rules
 * (unique ids, a listed instrument faces no one, no trade under a netting
 * set that is gone) are the server's (api/app/portfolio_ledger.py); these
 * helpers only build ledgers that respect them.
 */

export const RATINGS: Counterparty["rating"][] = [
	"AAA",
	"AA",
	"A",
	"BBB",
	"BB",
	"B",
	"CCC",
];

/** The sectors the regulatory CVA capital weighs a counterparty by. */
export const SECTORS: { value: Counterparty["sector"]; label: string }[] = [
	{ value: "sovereign", label: "Sovereign" },
	{ value: "local_government", label: "Local government" },
	{ value: "financial", label: "Financial" },
	{
		value: "basic_materials_energy_industrials",
		label: "Basic materials, energy, industrials",
	},
	{
		value: "consumer_transport_administrative",
		label: "Consumer goods, transport, administrative services",
	},
	{
		value: "technology_telecommunications",
		label: "Technology, telecommunications",
	},
	{
		value: "health_care_utilities_professional",
		label: "Health care, utilities, professional services",
	},
	{ value: "other", label: "Other" },
];

/** Variation margin as the margin rules for non-cleared derivatives set it. */
export const DEFAULT_CSA: CsaTerms = {
	threshold_counterparty: 0,
	threshold_bank: 0,
	minimum_transfer_amount: 500_000,
	margin_period_of_risk_days: 10,
	cashflows: "paid",
	initial_margin: false,
	initial_margin_model: "auto",
	initial_margin_today: null,
	collateral_rate_spread: 0,
};

const uid = () =>
	crypto.randomUUID?.() ?? Math.random().toString(36).slice(2, 14);

export function newNettingSet(name = "ISDA, uncollateralised"): NettingSet {
	return { id: uid(), name, csa: null };
}

export function newCounterparty(): Counterparty {
	return {
		id: uid(),
		name: "",
		rating: "BBB",
		sector: "other",
		recovery: 0.4,
		netting_sets: [newNettingSet()],
	};
}

const ledger = (pf: Portfolio, changes: Partial<Ledger>): Ledger => ({
	base_currency: pf.base_currency,
	instruments: pf.instruments ?? [],
	transactions: pf.transactions ?? [],
	counterparties: pf.counterparties ?? [],
	...changes,
});

/** The instruments traded under a netting set. */
export function tradedUnder(pf: Portfolio, nettingSetId: string): Instrument[] {
	return (pf.instruments ?? []).filter(
		(i) => i.netting_set_id === nettingSetId,
	);
}

/** A netting set of the portfolio, with the counterparty it belongs to. */
export function nettingSetOf(
	pf: Portfolio,
	nettingSetId: string | null | undefined,
): { counterparty: Counterparty; nettingSet: NettingSet } | null {
	for (const counterparty of pf.counterparties ?? []) {
		const nettingSet = counterparty.netting_sets?.find(
			(n) => n.id === nettingSetId,
		);
		if (nettingSet) return { counterparty, nettingSet };
	}
	return null;
}

/** Add a counterparty, or replace the one with the same id. */
export function saveCounterparty(pf: Portfolio, c: Counterparty): Ledger {
	const all = pf.counterparties ?? [];
	return ledger(pf, {
		counterparties: all.some((x) => x.id === c.id)
			? all.map((x) => (x.id === c.id ? c : x))
			: [...all, c],
	});
}

/**
 * The ledger without a counterparty — or null while a trade is still under
 * one of its netting sets: the trades are moved or closed first.
 */
export function removeCounterparty(pf: Portfolio, id: string): Ledger | null {
	const gone = (pf.counterparties ?? []).find((c) => c.id === id);
	if (gone?.netting_sets?.some((n) => tradedUnder(pf, n.id).length > 0))
		return null;
	return ledger(pf, {
		counterparties: (pf.counterparties ?? []).filter((c) => c.id !== id),
	});
}

/** The examples the portfolio does not already have, appended. */
export function addExamples(pf: Portfolio, examples: Counterparty[]): Ledger {
	const have = new Set((pf.counterparties ?? []).map((c) => c.id));
	return ledger(pf, {
		counterparties: [
			...(pf.counterparties ?? []),
			...examples.filter((e) => !have.has(e.id)),
		],
	});
}

/** Trade a derivative under a netting set (null: under none). */
export function assignNettingSet(
	pf: Portfolio,
	instrumentId: string,
	nettingSetId: string | null,
): Ledger {
	return ledger(pf, {
		instruments: (pf.instruments ?? []).map((i) =>
			i.id === instrumentId && i.spec.kind === "derivative"
				? { ...i, netting_set_id: nettingSetId }
				: i,
		),
	});
}

/** The collateral terms of a netting set in one line. */
export function describeCsa(csa: CsaTerms | null | undefined): string {
	if (!csa) return "Uncollateralised";
	const n = (v: number) => v.toLocaleString("en-US");
	return [
		`CSA: thresholds ${n(csa.threshold_counterparty)} (counterparty) / ${n(csa.threshold_bank)} (bank)`,
		`minimum transfer ${n(csa.minimum_transfer_amount)}`,
		`margin period of risk ${csa.margin_period_of_risk_days} business days`,
		csa.initial_margin
			? "initial margin posted both ways"
			: "no initial margin",
	].join(", ");
}
