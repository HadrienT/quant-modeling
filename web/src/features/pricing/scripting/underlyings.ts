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
	const setAsset = (i: number, patch: Partial<TypedAsset>) =>
		setAssets((xs) => xs.map((a, j) => (j === i ? { ...a, ...patch } : a)));
	return { tickers, setTickers, assets, setAsset, corrPct, setCorrPct };
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
			underlyings: tickerList(u.tickers)
				.slice(0, n)
				.map((ticker) => ({ ticker })),
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
