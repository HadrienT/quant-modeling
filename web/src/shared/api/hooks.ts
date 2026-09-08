import {
	type UseQueryOptions,
	useMutation,
	useQuery,
	useQueryClient,
} from "@tanstack/react-query";
import { LONG_TIMEOUT_MS, api, signalWithTimeout } from "./client";
import { ApiError } from "./errors";
import { queryKeys } from "./queryKeys";
import { STALE } from "./queryClient";
import type {
	BacktestRequest,
	BacktestResponse,
	CleanedIVSurfaceResponse,
	IVSurfaceResponse,
	LocalVolSurfaceResponse,
	MarketHistoryResponse,
	Portfolio,
	PortfolioSummary,
	RatesCurveResponse,
} from "./types";

/**
 * One hook per operation, named after the domain, not the route
 * (blueprint WP 02 §3). Invalidations live WITH the mutation that causes them.
 */

async function unwrap<T>(
	promise: Promise<{ data?: T; error?: unknown }>,
): Promise<T> {
	const { data, error } = await promise;
	if (error !== undefined) throw ApiError.from(error);
	if (data === undefined) {
		throw new ApiError({
			kind: "server",
			status: 500,
			code: "empty_response",
			message: "The server returned an empty response.",
		});
	}
	return data;
}

/* ── Health ────────────────────────────────────────────────────────────── */
export function useHealth() {
	return useQuery({
		queryKey: queryKeys.health(),
		queryFn: async ({ signal }) => {
			const s = signalWithTimeout(signal, 5_000);
			try {
				return await unwrap(api.GET("/health", { signal: s }));
			} finally {
				s.cleanup();
			}
		},
		refetchInterval: 30_000,
		staleTime: 15_000,
		retry: false,
	});
}

/* ── Market ────────────────────────────────────────────────────────────── */
export function useTickers() {
	return useQuery({
		queryKey: queryKeys.market.tickers(),
		queryFn: async ({ signal }) => {
			const s = signalWithTimeout(signal);
			try {
				return await unwrap(api.GET("/market/tickers", { signal: s }));
			} finally {
				s.cleanup();
			}
		},
		staleTime: STALE.history,
	});
}

export function usePriceHistory(
	ticker: string | null,
	range: string,
	options?: Partial<UseQueryOptions<MarketHistoryResponse, ApiError>>,
) {
	return useQuery({
		queryKey: queryKeys.market.history(ticker ?? "", range),
		enabled: Boolean(ticker),
		staleTime: STALE.history,
		queryFn: async ({ signal }) => {
			const s = signalWithTimeout(signal);
			try {
				return (await unwrap(
					api.GET("/market/prices/history", {
						params: { query: { ticker: ticker!, range } },
						signal: s,
					}),
				)) as MarketHistoryResponse;
			} finally {
				s.cleanup();
			}
		},
		...options,
	});
}

export function useIvSurface(
	ticker: string | null,
	surface: "mid" | "bid" | "ask" = "mid",
) {
	return useQuery({
		queryKey: queryKeys.market.ivSurface(ticker ?? "", surface),
		enabled: Boolean(ticker),
		staleTime: STALE.surface,
		queryFn: async ({ signal }) => {
			const s = signalWithTimeout(signal, LONG_TIMEOUT_MS);
			try {
				return (await unwrap(
					api.GET("/market/iv/surface", {
						params: { query: { ticker: ticker!, surface } },
						signal: s,
					}),
				)) as IVSurfaceResponse;
			} finally {
				s.cleanup();
			}
		},
	});
}

export function useCleanedIvSurface(ticker: string | null) {
	return useQuery({
		queryKey: queryKeys.market.cleanedIvSurface(ticker ?? ""),
		enabled: Boolean(ticker),
		staleTime: STALE.surface,
		queryFn: async ({ signal }) => {
			const s = signalWithTimeout(signal, LONG_TIMEOUT_MS);
			try {
				return (await unwrap(
					api.GET("/api/local-vol/iv-surface", {
						params: { query: { ticker: ticker! } },
						signal: s,
					}),
				)) as CleanedIVSurfaceResponse;
			} finally {
				s.cleanup();
			}
		},
	});
}

export function useLocalVolSurface(ticker: string | null) {
	return useQuery({
		queryKey: queryKeys.market.localVolSurface(ticker ?? ""),
		enabled: Boolean(ticker),
		staleTime: STALE.surface,
		queryFn: async ({ signal }) => {
			const s = signalWithTimeout(signal, LONG_TIMEOUT_MS);
			try {
				return (await unwrap(
					api.GET("/api/local-vol/surface", {
						params: { query: { ticker: ticker! } },
						signal: s,
					}),
				)) as LocalVolSurfaceResponse;
			} finally {
				s.cleanup();
			}
		},
	});
}

export function useRatesCurve(
	curve: "Treasury" | "SOFR" | "FedFunds",
	curveType: "zero" | "forward" = "zero",
	fixedPeriodYears = 0.5,
) {
	return useQuery({
		queryKey: queryKeys.market.ratesCurve(curve, curveType, fixedPeriodYears),
		staleTime: STALE.rates,
		queryFn: async ({ signal }) => {
			const s = signalWithTimeout(signal);
			try {
				return (await unwrap(
					api.GET("/market/rates/curve", {
						params: {
							query: {
								curve,
								curve_type: curveType,
								fixed_period_years: fixedPeriodYears,
							},
						},
						signal: s,
					}),
				)) as RatesCurveResponse;
			} finally {
				s.cleanup();
			}
		},
	});
}

/* ── Portfolio ─────────────────────────────────────────────────────────── */
export function usePortfolios() {
	return useQuery({
		queryKey: queryKeys.portfolio.list(),
		staleTime: STALE.portfolio,
		queryFn: async ({ signal }) => {
			const s = signalWithTimeout(signal);
			try {
				return (await unwrap(
					api.GET("/api/portfolios", { signal: s }),
				)) as PortfolioSummary[];
			} finally {
				s.cleanup();
			}
		},
	});
}

export function usePortfolio(id: string | null) {
	return useQuery({
		queryKey: queryKeys.portfolio.detail(id ?? ""),
		enabled: Boolean(id),
		staleTime: STALE.portfolio,
		queryFn: async ({ signal }) => {
			const s = signalWithTimeout(signal);
			try {
				return (await unwrap(
					api.GET("/api/portfolios/{portfolio_id}", {
						params: { path: { portfolio_id: id! } },
						signal: s,
					}),
				)) as Portfolio;
			} finally {
				s.cleanup();
			}
		},
	});
}

/** Batch-price a portfolio; invalidates its positions, risk and VaR — not the market. */
export function usePricePortfolio() {
	const qc = useQueryClient();
	return useMutation({
		mutationFn: async (portfolioId: string) => {
			const s = signalWithTimeout(undefined, LONG_TIMEOUT_MS);
			try {
				return await unwrap(
					api.POST("/api/portfolios/{portfolio_id}/price", {
						params: { path: { portfolio_id: portfolioId } },
						signal: s,
					}),
				);
			} finally {
				s.cleanup();
			}
		},
		onSuccess: (_data, portfolioId) => {
			void qc.invalidateQueries({
				queryKey: queryKeys.portfolio.detail(portfolioId),
			});
			void qc.invalidateQueries({
				queryKey: queryKeys.portfolio.risk(portfolioId),
			});
			void qc.invalidateQueries({
				queryKey: ["portfolio", "var", portfolioId],
			});
			void qc.invalidateQueries({ queryKey: queryKeys.portfolio.list() });
		},
	});
}

/* ── Backtest ──────────────────────────────────────────────────────────── */
export function useRunBacktest() {
	const qc = useQueryClient();
	return useMutation<BacktestResponse, ApiError, BacktestRequest>({
		mutationFn: async (req) => {
			const s = signalWithTimeout(undefined, LONG_TIMEOUT_MS);
			try {
				return (await unwrap(
					api.POST("/api/backtest/run", { body: req, signal: s }),
				)) as BacktestResponse;
			} finally {
				s.cleanup();
			}
		},
		onSuccess: (data, req) => {
			// Cache the result so an identical re-run does not recompute.
			qc.setQueryData(queryKeys.backtest.run(req), data);
		},
	});
}
