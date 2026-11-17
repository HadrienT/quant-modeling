import { useSyncExternalStore } from "react";

/**
 * The underlying the visitor last chose explicitly, and its market (WP 28,
 * lot U4): Market, Credit and Simulation start on it when their URL names
 * none, and the bar shows it. An explicit URL parameter always wins. Kept in
 * localStorage, read and written inside try/catch: a private window or
 * blocked storage only loses the convenience, never a page.
 */
export type MarketContext = { ticker: string; market: string };

const KEY = "qm_market_context";
const listeners = new Set<() => void>();
let memory: MarketContext | null = null;
let cachedRaw: string | null | undefined;

function read(): MarketContext | null {
	let raw: string | null = null;
	try {
		raw = localStorage.getItem(KEY);
	} catch {
		return memory;
	}
	if (raw === cachedRaw) return memory;
	cachedRaw = raw;
	try {
		const v = raw ? (JSON.parse(raw) as Partial<MarketContext>) : null;
		memory =
			v && typeof v.ticker === "string" && typeof v.market === "string"
				? { ticker: v.ticker, market: v.market }
				: null;
	} catch {
		memory = null;
	}
	return memory;
}

/** Remember an underlying the visitor picked; a no-op if it is already the one. */
export function rememberTicker(ticker: string, market = "SP500") {
	const t = ticker.trim().toUpperCase();
	if (!t) return;
	const cur = read();
	if (cur?.ticker === t && cur.market === market) return;
	memory = { ticker: t, market };
	try {
		cachedRaw = JSON.stringify(memory);
		localStorage.setItem(KEY, cachedRaw);
	} catch {
		/* storage blocked: kept in memory for this tab */
	}
	listeners.forEach((l) => l());
}

function subscribe(l: () => void) {
	listeners.add(l);
	const onStorage = (e: StorageEvent) => e.key === KEY && l();
	window.addEventListener("storage", onStorage);
	return () => {
		listeners.delete(l);
		window.removeEventListener("storage", onStorage);
	};
}

export function useMarketContext(): MarketContext | null {
	return useSyncExternalStore(subscribe, read, () => null);
}

/** The test suite's reset: forget the context between tests. */
export function __resetMarketContext() {
	memory = null;
	cachedRaw = undefined;
}
