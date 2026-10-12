import { keepPreviousData, useQuery } from "@tanstack/react-query";
import { LONG_TIMEOUT_MS, api, signalWithTimeout } from "./client";
import { ApiError } from "./errors";
import { queryKeys } from "./queryKeys";
import type {
	BacktestStrategy,
	BacktestStrategyRequest,
	BacktestUniverse,
	BacktestValidation,
	BacktestValidationRequest,
} from "./types";

/**
 * The backtest page (api/app/routers/backtest.py). A run is a function of its
 * request (the seed of the bootstrap is part of it) on data that changes once
 * a day: a request is its own key, a result stays fresh for the session, and
 * nothing is retried, since a refusal of the engine is an answer.
 */

async function call<T>(
	signal: AbortSignal | undefined,
	run: (s: AbortSignal) => Promise<{ data?: unknown; error?: unknown }>,
): Promise<T> {
	const s = signalWithTimeout(signal, LONG_TIMEOUT_MS);
	try {
		const { data, error } = await run(s);
		if (error !== undefined) throw ApiError.from(error);
		return data as T;
	} finally {
		s.cleanup();
	}
}

/** What can be backtested, and the build of the engine that runs it. */
export function useBacktestUniverse() {
	return useQuery<BacktestUniverse, ApiError>({
		queryKey: queryKeys.backtest.universe(),
		staleTime: Infinity,
		queryFn: ({ signal }) =>
			call<BacktestUniverse>(signal, (s) =>
				api.GET("/api/backtest/universe", { signal: s }),
			),
	});
}

/** A strategy with its comparison points, metrics and intervals (null:
 * the universe, which gives the last day of data, is not loaded yet). */
export function useBacktestStrategy(request: BacktestStrategyRequest | null) {
	return useQuery<BacktestStrategy, ApiError>({
		queryKey: queryKeys.backtest.strategy(request),
		enabled: request != null,
		staleTime: Infinity,
		retry: false,
		placeholderData: keepPreviousData,
		queryFn: ({ signal }) =>
			call<BacktestStrategy>(signal, (s) =>
				api.POST("/api/backtest/strategy", {
					body: request as BacktestStrategyRequest,
					signal: s,
				}),
			),
	});
}

/** A grid of configurations against overfitting (null: not asked for). */
export function useBacktestValidation(
	request: BacktestValidationRequest | null,
) {
	return useQuery<BacktestValidation, ApiError>({
		queryKey: queryKeys.backtest.validation(request),
		enabled: request != null,
		staleTime: Infinity,
		retry: false,
		placeholderData: keepPreviousData,
		queryFn: ({ signal }) =>
			call<BacktestValidation>(signal, (s) =>
				api.POST("/api/backtest/validation", {
					body: request as BacktestValidationRequest,
					signal: s,
				}),
			),
	});
}
