import {
	type UseQueryOptions,
	useMutation,
	useQuery,
	useQueryClient,
	keepPreviousData,
} from "@tanstack/react-query";
import { LONG_TIMEOUT_MS, api, signalWithTimeout } from "./client";
import { ApiError } from "./errors";
import { queryKeys } from "./queryKeys";
import { STALE } from "./queryClient";
import type {
	BacktestRequest,
	BacktestResponse,
	BSPathRequest,
	CleanedIVSurfaceResponse,
	DeltaSurfaceResponse,
	LocalVolSurfaceResponse,
	MarketHistoryResponse,
	MarketId,
	FxCorrelationResponse,
	FxCurrency,
	FxHistoryResponse,
	FxOverviewResponse,
	Portfolio,
	PortfolioSummary,
	RateCurrency,
	RatesHistoryResponse,
	RatesOverviewResponse,
	ModelPathRequest,
	SABRPathRequest,
	SimulationCalibrateRequest,
	SimulationCalibrateResponse,
	SimulationPathsResponse,
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
/** Without a market: the historical S&P 500 + index-ETF list (backtest). */
export function useTickers(market?: MarketId) {
	return useQuery({
		queryKey: queryKeys.market.tickers(market),
		queryFn: async ({ signal }) => {
			const s = signalWithTimeout(signal);
			try {
				return await unwrap(
					api.GET("/market/tickers", {
						params: { query: market ? { market } : {} },
						signal: s,
					}),
				);
			} finally {
				s.cleanup();
			}
		},
		staleTime: STALE.history,
	});
}

export function useMarkets() {
	return useQuery({
		queryKey: queryKeys.market.markets(),
		queryFn: async ({ signal }) => {
			const s = signalWithTimeout(signal);
			try {
				return await unwrap(api.GET("/market/markets", { signal: s }));
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

export function useRawIvSurface(ticker: string | null) {
	return useQuery({
		queryKey: queryKeys.market.rawIvSurface(ticker ?? ""),
		enabled: Boolean(ticker),
		staleTime: STALE.surface,
		queryFn: async ({ signal }) => {
			const s = signalWithTimeout(signal, LONG_TIMEOUT_MS);
			try {
				return (await unwrap(
					api.GET("/api/local-vol/raw-surface", {
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

export function useDeltaSurface(ticker: string | null) {
	return useQuery({
		queryKey: queryKeys.market.deltaSurface(ticker ?? ""),
		enabled: Boolean(ticker),
		staleTime: STALE.surface,
		queryFn: async ({ signal }) => {
			const s = signalWithTimeout(signal, LONG_TIMEOUT_MS);
			try {
				return (await unwrap(
					api.GET("/api/local-vol/delta-surface", {
						params: { query: { ticker: ticker! } },
						signal: s,
					}),
				)) as DeltaSurfaceResponse;
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

export function useRatesOverview(
	currency: RateCurrency,
	forwardPeriodYears = 0.5,
) {
	return useQuery({
		queryKey: queryKeys.market.ratesOverview(currency, forwardPeriodYears),
		staleTime: STALE.rates,
		// Keep the page on screen while another currency or forward period loads.
		placeholderData: keepPreviousData,
		queryFn: async ({ signal }) => {
			const s = signalWithTimeout(signal);
			try {
				return (await unwrap(
					api.GET("/market/rates/overview", {
						params: {
							query: { currency, forward_period_years: forwardPeriodYears },
						},
						signal: s,
					}),
				)) as RatesOverviewResponse;
			} finally {
				s.cleanup();
			}
		},
	});
}

export function useRatesHistory(
	currency: RateCurrency,
	seriesId: string | undefined,
	years = 5,
) {
	return useQuery({
		queryKey: queryKeys.market.ratesHistory(currency, seriesId ?? "", years),
		enabled: !!seriesId,
		staleTime: STALE.rates,
		queryFn: async ({ signal }) => {
			const s = signalWithTimeout(signal);
			try {
				return (await unwrap(
					api.GET("/market/rates/history", {
						params: { query: { currency, series_id: seriesId!, years } },
						signal: s,
					}),
				)) as RatesHistoryResponse;
			} finally {
				s.cleanup();
			}
		},
	});
}

export function useFxOverview(base: FxCurrency, quote: FxCurrency) {
	return useQuery({
		queryKey: queryKeys.market.fxOverview(base, quote),
		staleTime: STALE.rates,
		enabled: base !== quote,
		queryFn: async ({ signal }) => {
			const s = signalWithTimeout(signal);
			try {
				return (await unwrap(
					api.GET("/market/fx/overview", {
						params: { query: { base, quote } },
						signal: s,
					}),
				)) as FxOverviewResponse;
			} finally {
				s.cleanup();
			}
		},
	});
}

export function useFxHistory(base: FxCurrency, quote: FxCurrency, years = 5) {
	return useQuery({
		queryKey: queryKeys.market.fxHistory(base, quote, years),
		staleTime: STALE.rates,
		enabled: base !== quote,
		queryFn: async ({ signal }) => {
			const s = signalWithTimeout(signal);
			try {
				return (await unwrap(
					api.GET("/market/fx/history", {
						params: { query: { base, quote, years } },
						signal: s,
					}),
				)) as FxHistoryResponse;
			} finally {
				s.cleanup();
			}
		},
	});
}

export type FxCorrelationQuery = {
	ticker: string;
	base: FxCurrency;
	quote: FxCurrency;
	window: "1Y" | "3Y" | "5Y";
	frequency: "weekly" | "daily";
};

export function useFxCorrelation(q: FxCorrelationQuery | null) {
	return useQuery({
		queryKey: queryKeys.market.fxCorrelation(JSON.stringify(q)),
		staleTime: STALE.rates,
		enabled: !!q && !!q.ticker && q.base !== q.quote,
		queryFn: async ({ signal }) => {
			const s = signalWithTimeout(signal);
			try {
				return (await unwrap(
					api.GET("/market/fx/correlation", {
						params: { query: q! },
						signal: s,
					}),
				)) as FxCorrelationResponse;
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

/* ── Simulation ────────────────────────────────────────────────────────── */
export function useSimulateBlackScholesPaths() {
	return useMutation<SimulationPathsResponse, ApiError, BSPathRequest>({
		mutationFn: async (req) => {
			const s = signalWithTimeout(undefined, LONG_TIMEOUT_MS);
			try {
				return (await unwrap(
					api.POST("/api/simulation/paths/black-scholes", {
						body: req,
						signal: s,
					}),
				)) as SimulationPathsResponse;
			} finally {
				s.cleanup();
			}
		},
	});
}

export function useSimulateSabrPaths() {
	return useMutation<SimulationPathsResponse, ApiError, SABRPathRequest>({
		mutationFn: async (req) => {
			const s = signalWithTimeout(undefined, LONG_TIMEOUT_MS);
			try {
				return (await unwrap(
					api.POST("/api/simulation/paths/sabr", { body: req, signal: s }),
				)) as SimulationPathsResponse;
			} finally {
				s.cleanup();
			}
		},
	});
}

/** Local vol, Heston or SLV paths (POST /api/simulation/paths/model). */
export function useSimulateModelPaths() {
	return useMutation<SimulationPathsResponse, ApiError, ModelPathRequest>({
		mutationFn: async (req) => {
			const s = signalWithTimeout(undefined, LONG_TIMEOUT_MS);
			try {
				return (await unwrap(
					api.POST("/api/simulation/paths/model", { body: req, signal: s }),
				)) as SimulationPathsResponse;
			} finally {
				s.cleanup();
			}
		},
	});
}

export function useCalibrateSimulation() {
	return useMutation<
		SimulationCalibrateResponse,
		ApiError,
		SimulationCalibrateRequest
	>({
		mutationFn: async (req) => {
			const s = signalWithTimeout(undefined, LONG_TIMEOUT_MS);
			try {
				return (await unwrap(
					api.POST("/api/simulation/calibrate", { body: req, signal: s }),
				)) as SimulationCalibrateResponse;
			} finally {
				s.cleanup();
			}
		},
	});
}
