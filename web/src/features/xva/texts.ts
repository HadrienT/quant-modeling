/**
 * The formulas of the page, in Gregory's notation and sign convention (The
 * xVA Challenge, 4th ed.): an adjustment is negative when it costs the bank.
 * D is the discount factor, V the value of the netting set to the bank, S the
 * survival probability of a party (C the counterparty, I the bank).
 */

export const GREGORY =
	"Gregory, The xVA Challenge: Counterparty Risk, Funding, Collateral, Capital and Initial Margin, 4th ed., Wiley, 2020";

export const EXPOSURE_TEX = String.raw`EE(t) = \mathbb{E}\big[\max(V(t), 0)\big], \qquad ENE(t) = \mathbb{E}\big[\min(V(t), 0)\big], \qquad PFE_\alpha(t) = q_\alpha\big(V(t)\big)^+`;

export const EPE_TEX = String.raw`EPE = \frac{1}{T}\int_0^T EE(t)\,dt, \qquad EEPE = \frac{1}{1\text{y}}\int_0^{1\text{y}} \max_{s \le t} EE(s)\,dt`;

export type AdjustmentKey = "cva" | "dva" | "fva" | "colva" | "mva" | "kva";

export const ADJUSTMENTS: {
	key: AdjustmentKey;
	label: string;
	name: string;
	tex: string;
	text: string;
	source: string;
}[] = [
	{
		key: "cva",
		label: "CVA",
		name: "Credit valuation adjustment",
		tex: String.raw`CVA = -LGD_C \sum_i EE^*(t_i)\, S_I(t_{i-1})\,\big[S_C(t_{i-1}) - S_C(t_i)\big]`,
		text: "What the counterparty's default is expected to cost: at each date, the discounted expected exposure times the probability that the counterparty defaults then, the bank still being alive, times the share that is not recovered.",
		source: GREGORY,
	},
	{
		key: "dva",
		label: "DVA",
		name: "Debit valuation adjustment",
		tex: String.raw`DVA = -LGD_I \sum_i ENE^*(t_i)\, S_C(t_{i-1})\,\big[S_I(t_{i-1}) - S_I(t_i)\big]`,
		text: "The same thing seen by the counterparty about the bank: what the bank would not pay back if it defaulted first. A gain for the bank, which is why regulatory capital ignores it.",
		source: GREGORY,
	},
	{
		key: "fva",
		label: "FVA",
		name: "Funding valuation adjustment",
		tex: String.raw`FVA = -\sum_i \big[s_B\, EE^*(t_i) + s_L\, ENE^*(t_i)\big]\, S_C(t_i)\, S_I(t_i)\, \Delta t_i`,
		text: "An uncollateralised asset has to be funded at the bank's borrowing spread, and a liability funds the bank at its lending spread: the cost of the first (FCA) and the benefit of the second (FBA), while both parties are alive.",
		source:
			"Burgard & Kjaer, Partial differential equation representations of derivatives with bilateral counterparty risk and funding costs, Journal of Credit Risk, 2011; " +
			GREGORY,
	},
	{
		key: "colva",
		label: "ColVA",
		name: "Collateral valuation adjustment",
		tex: String.raw`ColVA = -\sum_i \mathbb{E}\big[D(t_i)\, C(t_i)\big]\, S_C(t_i)\, S_I(t_i)\, s_{col}\, \Delta t_i`,
		text: "The collateral held earns the rate the agreement specifies. When that rate is not the discount rate, the difference is paid or received on the collateral balance for the life of the trades.",
		source: "Piterbarg, Funding beyond discounting, Risk, 2010; " + GREGORY,
	},
	{
		key: "mva",
		label: "MVA",
		name: "Margin valuation adjustment",
		tex: String.raw`MVA = -\sum_i \mathbb{E}\big[D(t_i)\, IM(t_i)\big]\, S_C(t_i)\, S_I(t_i)\, s_B\, \Delta t_i`,
		text: "Initial margin is posted and held apart: it cannot be reused, so it is funded at the bank's borrowing spread for as long as it is posted. Initial margin turns most of the CVA into this cost.",
		source:
			"Green & Kenyon, MVA by replication and regression, Risk, 2015; " +
			GREGORY,
	},
	{
		key: "kva",
		label: "KVA",
		name: "Capital valuation adjustment",
		tex: String.raw`KVA = -\sum_i \mathbb{E}\big[D(t_i)\, K(t_i)\big]\, S_C(t_i)\, S_I(t_i)\, c_K\, \Delta t_i`,
		text: "The trades tie up regulatory capital for their whole life — against the counterparty's default and against moves of the CVA — and shareholders want a return on it.",
		source:
			"Green, Kenyon & Dennis, KVA: capital valuation adjustment by replication, Risk, 2014; " +
			GREGORY,
	},
];
