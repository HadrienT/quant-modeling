import type { Leg, StrategyKey } from "./types";

type StrategyDef = {
	label: string;
	desc: string;
	buildLegs: (atm: number) => Omit<Leg, "id">[];
};

export const STRATEGIES: Record<StrategyKey, StrategyDef> = {
	custom: {
		label: "Custom",
		desc: "Build your own",
		buildLegs: () => [],
	},
	"long-call": {
		label: "Long Call",
		desc: "Bullish, limited risk",
		buildLegs: (atm) => [
			{ label: "Call", type: "call", side: "long", strike: atm, premium: 5, quantity: 1, cashAmount: 1 },
		],
	},
	"long-put": {
		label: "Long Put",
		desc: "Bearish, limited risk",
		buildLegs: (atm) => [
			{ label: "Put", type: "put", side: "long", strike: atm, premium: 5, quantity: 1, cashAmount: 1 },
		],
	},
	"covered-call": {
		label: "Covered Call",
		desc: "Long stock + short OTM call",
		buildLegs: (atm) => [
			{ label: "Stock", type: "stock", side: "long", strike: atm, premium: 0, quantity: 1, cashAmount: 1 },
			{ label: "Short Call", type: "call", side: "short", strike: atm + 10, premium: 3, quantity: 1, cashAmount: 1 },
		],
	},
	"protective-put": {
		label: "Protective Put",
		desc: "Long stock + long OTM put",
		buildLegs: (atm) => [
			{ label: "Stock", type: "stock", side: "long", strike: atm, premium: 0, quantity: 1, cashAmount: 1 },
			{ label: "Put", type: "put", side: "long", strike: atm - 10, premium: 2.5, quantity: 1, cashAmount: 1 },
		],
	},
	straddle: {
		label: "Straddle",
		desc: "Long call + put, same strike",
		buildLegs: (atm) => [
			{ label: "Call", type: "call", side: "long", strike: atm, premium: 5, quantity: 1, cashAmount: 1 },
			{ label: "Put", type: "put", side: "long", strike: atm, premium: 5, quantity: 1, cashAmount: 1 },
		],
	},
	strangle: {
		label: "Strangle",
		desc: "Long OTM call + OTM put",
		buildLegs: (atm) => [
			{ label: "OTM Call", type: "call", side: "long", strike: atm + 10, premium: 3, quantity: 1, cashAmount: 1 },
			{ label: "OTM Put", type: "put", side: "long", strike: atm - 10, premium: 3, quantity: 1, cashAmount: 1 },
		],
	},
	"bull-call-spread": {
		label: "Bull Spread",
		desc: "Long lower call + short higher call",
		buildLegs: (atm) => [
			{ label: "Long Call", type: "call", side: "long", strike: atm - 5, premium: 7, quantity: 1, cashAmount: 1 },
			{ label: "Short Call", type: "call", side: "short", strike: atm + 5, premium: 3, quantity: 1, cashAmount: 1 },
		],
	},
	"bear-put-spread": {
		label: "Bear Spread",
		desc: "Long higher put + short lower put",
		buildLegs: (atm) => [
			{ label: "Long Put", type: "put", side: "long", strike: atm + 5, premium: 7, quantity: 1, cashAmount: 1 },
			{ label: "Short Put", type: "put", side: "short", strike: atm - 5, premium: 3, quantity: 1, cashAmount: 1 },
		],
	},
	butterfly: {
		label: "Butterfly",
		desc: "Low call + 2× short ATM + high call",
		buildLegs: (atm) => [
			{ label: "Low Call", type: "call", side: "long", strike: atm - 10, premium: 8, quantity: 1, cashAmount: 1 },
			{ label: "ATM Call ×2", type: "call", side: "short", strike: atm, premium: 5, quantity: 2, cashAmount: 1 },
			{ label: "High Call", type: "call", side: "long", strike: atm + 10, premium: 3, quantity: 1, cashAmount: 1 },
		],
	},
	"iron-condor": {
		label: "Iron Condor",
		desc: "Short strangle + long wings",
		buildLegs: (atm) => [
			{ label: "Long Put", type: "put", side: "long", strike: atm - 15, premium: 1.5, quantity: 1, cashAmount: 1 },
			{ label: "Short Put", type: "put", side: "short", strike: atm - 5, premium: 3.5, quantity: 1, cashAmount: 1 },
			{ label: "Short Call", type: "call", side: "short", strike: atm + 5, premium: 3.5, quantity: 1, cashAmount: 1 },
			{ label: "Long Call", type: "call", side: "long", strike: atm + 15, premium: 1.5, quantity: 1, cashAmount: 1 },
		],
	},
	collar: {
		label: "Collar",
		desc: "Stock + put + short call",
		buildLegs: (atm) => [
			{ label: "Stock", type: "stock", side: "long", strike: atm, premium: 0, quantity: 1, cashAmount: 1 },
			{ label: "Put", type: "put", side: "long", strike: atm - 10, premium: 2, quantity: 1, cashAmount: 1 },
			{ label: "Short Call", type: "call", side: "short", strike: atm + 10, premium: 2, quantity: 1, cashAmount: 1 },
		],
	},
};

export const STRATEGY_KEYS = Object.keys(STRATEGIES) as StrategyKey[];
