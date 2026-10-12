import type { RateFxDraft } from "@/shared/api";

export type Product = RateFxDraft["product"];

/** What the two sides and the rate of each product are called. */
export const PRODUCTS: Record<
	Product,
	{ label: string; sides: [string, string]; rate: string; percent: boolean }
> = {
	swap: {
		label: "Interest rate swap",
		sides: ["Pay fixed", "Receive fixed"],
		rate: "Fixed rate (%)",
		percent: true,
	},
	swaption: {
		label: "European swaption",
		sides: ["On a payer swap", "On a receiver swap"],
		rate: "Strike (%)",
		percent: true,
	},
	bermudan: {
		label: "Bermudan swaption",
		sides: ["On a payer swap", "On a receiver swap"],
		rate: "Strike (%)",
		percent: true,
	},
	fx_forward: {
		label: "FX forward EUR/USD",
		sides: ["Buy the euros", "Sell the euros"],
		rate: "Delivery rate (USD per EUR)",
		percent: false,
	},
	cross_currency_swap: {
		label: "Cross-currency swap EUR/USD",
		sides: ["Receive EUR, pay USD", "Pay EUR, receive USD"],
		rate: "USD coupon (%)",
		percent: true,
	},
};

export const plusYears = (iso: string, years: number) => {
	const [y, m, d] = iso.split("-").map(Number);
	// 29 February has no anniversary three years out of four: the 28th.
	const day = m === 2 && d === 29 && (y! + years) % 4 !== 0 ? 28 : d!;
	return `${y! + years}-${String(m).padStart(2, "0")}-${String(day).padStart(2, "0")}`;
};
