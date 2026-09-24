import { keepPreviousData, useQuery } from "@tanstack/react-query";
import { LONG_TIMEOUT_MS, api, signalWithTimeout } from "./client";
import { ApiError } from "./errors";
import { queryKeys } from "./queryKeys";
import { STALE } from "./queryClient";
import type {
	Portfolio,
	PortfolioHistory,
	PortfolioSnapshot,
	PnlWindow,
	TickerClose,
} from "./types";

/**
 * Portfolio valuation (api/app/routers/portfolio_valuation.py). The endpoints
 * are stateless — the ledger travels in the body — so a portfolio kept in
 * this browser is valued exactly like one stored on the server.
 */

async function unwrap<T>(
	promise: Promise<{ data?: T; error?: unknown }>,
): Promise<T> {
	const { data, error } = await promise;
	if (error !== undefined) throw ApiError.from(error);
	return data as T;
}

/** The part of a portfolio its valuation depends on (the cache key). */
function ledgerKey(pf: Portfolio | null | undefined) {
	return pf
		? JSON.stringify([
				pf.base_currency,
				pf.instruments ?? [],
				pf.transactions ?? [],
				pf.positions ?? [],
			])
		: "";
}

const hasTrades = (pf: Portfolio | null | undefined) =>
	!!pf &&
	((pf.transactions?.length ?? 0) > 0 || (pf.positions?.length ?? 0) > 0);

/** Every position marked to market today, the totals and today's P&L. */
export function usePortfolioSnapshot(pf: Portfolio | null | undefined) {
	return useQuery({
		queryKey: queryKeys.portfolio.snapshot(ledgerKey(pf)),
		enabled: hasTrades(pf),
		staleTime: STALE.portfolio,
		placeholderData: keepPreviousData,
		queryFn: async ({ signal }) => {
			const s = signalWithTimeout(signal, LONG_TIMEOUT_MS);
			try {
				return await unwrap<PortfolioSnapshot>(
					api.POST("/api/portfolio-valuation/snapshot", {
						body: { portfolio: pf! as never },
						signal: s,
					}),
				);
			} finally {
				s.cleanup();
			}
		},
	});
}

/** Daily P&L over a window: each past day valued with that day's market. */
export function usePortfolioHistory(
	pf: Portfolio | null | undefined,
	window: PnlWindow,
) {
	return useQuery({
		queryKey: queryKeys.portfolio.history(ledgerKey(pf), window),
		enabled: hasTrades(pf),
		staleTime: STALE.portfolio,
		placeholderData: keepPreviousData,
		queryFn: async ({ signal }) => {
			const s = signalWithTimeout(signal, LONG_TIMEOUT_MS);
			try {
				return await unwrap<PortfolioHistory>(
					api.POST("/api/portfolio-valuation/history", {
						body: { portfolio: pf! as never, window },
						signal: s,
					}),
				);
			} finally {
				s.cleanup();
			}
		},
	});
}

/** A ticker's close on or before a date — the trade form's default price. */
export function useTickerClose(ticker: string, date: string) {
	return useQuery({
		queryKey: queryKeys.portfolio.close(ticker, date),
		enabled: !!ticker && !!date,
		staleTime: STALE.history,
		retry: false,
		queryFn: async ({ signal }) => {
			const s = signalWithTimeout(signal);
			try {
				return await unwrap<TickerClose>(
					api.GET("/api/portfolio-valuation/close", {
						params: { query: { ticker, date } },
						signal: s,
					}),
				);
			} finally {
				s.cleanup();
			}
		},
	});
}

/** A position-based portfolio (before the ledger) rewritten as a ledger. */
export async function migratePortfolio(pf: Portfolio): Promise<Portfolio> {
	return unwrap<Portfolio>(
		api.POST("/api/portfolio-valuation/migrate", {
			body: { portfolio: pf as never },
		}),
	);
}
