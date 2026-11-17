import { keepPreviousData, useMutation, useQuery } from "@tanstack/react-query";
import { api } from "./client";
import type { ApiError } from "./errors";
import { longCall as call } from "./longCall";
import { queryKeys } from "./queryKeys";
import type {
	BacktestHedgingRequest,
	BacktestTask,
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

/** Start a hedging backtest: a task, whose id comes back at once (a Heston
 * world takes minutes). */
export function useStartHedging() {
	return useMutation<BacktestTask, ApiError, BacktestHedgingRequest>({
		mutationFn: (body) =>
			call<BacktestTask>(undefined, (s) =>
				api.POST("/api/backtest/hedging", { body, signal: s }),
			),
	});
}

/** A task's progress, polled every second while it is queued or running,
 * then its result (null: no task). */
export function useBacktestTask(id: string | null) {
	return useQuery<BacktestTask, ApiError>({
		queryKey: queryKeys.backtest.task(id),
		enabled: id != null,
		retry: false,
		refetchInterval: (q) => {
			const status = q.state.data?.status;
			return status === "queued" || status === "running" ? 1000 : false;
		},
		queryFn: ({ signal }) =>
			call<BacktestTask>(signal, (s) =>
				api.GET("/api/backtest/tasks/{task_id}", {
					params: { path: { task_id: id as string } },
					signal: s,
				}),
			),
	});
}

/** Cancel a task: a queued one never starts, a running one stops at the
 * engine's next progress report. */
export function useCancelBacktestTask() {
	return useMutation<BacktestTask, ApiError, string>({
		mutationFn: (id) =>
			call<BacktestTask>(undefined, (s) =>
				api.DELETE("/api/backtest/tasks/{task_id}", {
					params: { path: { task_id: id } },
					signal: s,
				}),
			),
	});
}
