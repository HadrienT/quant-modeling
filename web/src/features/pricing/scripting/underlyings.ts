import { useState } from "react";

/** How many underlyings a script reads: the highest spot(i) index plus one
 * (spot() is spot(0)). Comments are ignored. */
export function countUnderlyings(script: string): number {
	const code = script.replace(/#.*$/gm, "");
	let n = 1;
	for (const m of code.matchAll(/spot\(\s*(\d+)\s*\)/g))
		n = Math.max(n, Number(m[1]) + 1);
	return n;
}

export type TypedAsset = { spot: string; volPct: string; divPct: string };

const MAX_ASSETS = 8;

/** Inputs of a multi-asset script: tickers (market data) or typed flat
 * Black-Scholes inputs with one correlation for every pair. */
export function useUnderlyings() {
	const [tickers, setTickers] = useState("AAPL, MSFT, GOOGL, AMZN");
	const [assets, setAssets] = useState<TypedAsset[]>(() =>
		Array.from({ length: MAX_ASSETS }, () => ({
			spot: "100",
			volPct: "25",
			divPct: "0",
		})),
	);
	const [corrPct, setCorrPct] = useState("50");
	/** Payment currency; "" = the listing currency of the first ticker. */
	const [currency, setCurrency] = useState("");
	const setAsset = (i: number, patch: Partial<TypedAsset>) =>
		setAssets((xs) => xs.map((a, j) => (j === i ? { ...a, ...patch } : a)));
	return {
		tickers,
		setTickers,
		assets,
		setAsset,
		corrPct,
		setCorrPct,
		currency,
		setCurrency,
	};
}

/** Payment currencies with a stored curve and ECB fixings. */
export const PAYMENT_CURRENCIES = ["USD", "EUR", "JPY"] as const;

/** One entry of the tickers field: a ticker, or FX:EUR for an exchange
 * rate (one euro in the payment currency). */
export function underlyingEntry(
	token: string,
): { ticker: string } | { fx: string } {
	const m = /^FX:([A-Z]{3})$/.exec(token);
	return m?.[1] ? { fx: m[1] } : { ticker: token };
}

export type Underlyings = ReturnType<typeof useUnderlyings>;

export function tickerList(tickers: string): string[] {
	return tickers
		.split(/[\s,;]+/)
		.map((t) => t.trim().toUpperCase())
		.filter(Boolean);
}

/** The request fields for `n` underlyings: tickers when the model reads the
 * market, typed inputs otherwise. */
export function underlyingsRequest(
	n: number,
	market: boolean,
	u: Underlyings,
): Record<string, unknown> {
	if (market)
		return {
			underlyings: tickerList(u.tickers).slice(0, n).map(underlyingEntry),
			...(u.currency ? { currency: u.currency } : {}),
		};
	const rho = Number(u.corrPct) / 100;
	return {
		underlyings: u.assets.slice(0, n).map((a) => ({
			spot: Number(a.spot),
			vol: Number(a.volPct) / 100,
			dividend: Number(a.divPct) / 100,
		})),
		correlation: Array.from({ length: n }, (_, i) =>
			Array.from({ length: n }, (_, j) => (i === j ? 1 : rho)),
		),
	};
}
