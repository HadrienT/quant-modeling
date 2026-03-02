export type LegType = "call" | "put" | "stock" | "forward" | "digital-call" | "digital-put";
export type LegSide = "long" | "short";

export type Leg = {
	id: string;
	label: string;
	type: LegType;
	side: LegSide;
	strike: number;
	premium: number;
	quantity: number;
	cashAmount: number;
};

export const LEG_TYPE_LABELS: Record<LegType, string> = {
	call: "Call",
	put: "Put",
	stock: "Stock",
	forward: "Forward",
	"digital-call": "Digital Call",
	"digital-put": "Digital Put",
};

export type StrategyKey =
	| "custom"
	| "long-call"
	| "long-put"
	| "covered-call"
	| "protective-put"
	| "bull-call-spread"
	| "bear-put-spread"
	| "straddle"
	| "strangle"
	| "butterfly"
	| "iron-condor"
	| "collar";

export type Metrics = {
	maxProfit: number;
	maxLoss: number;
	breakevens: number[];
	netPremium: number;
};
