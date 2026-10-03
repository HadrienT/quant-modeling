import { keepPreviousData, useQuery } from "@tanstack/react-query";
import { LONG_TIMEOUT_MS, api, signalWithTimeout } from "./client";
import { ApiError } from "./errors";
import { queryKeys } from "./queryKeys";
import { STALE } from "./queryClient";
import type {
	XvaPortfolios,
	XvaRequest,
	XvaResponse,
	XvaSensitivities,
	XvaSensitivitiesRequest,
} from "./types";

/**
 * The xVA page (api/app/routers/xva.py): the exposure and the adjustments of
 * a netting set on market data, and their sensitivities by adjoint
 * differentiation. Both are simulations of a few seconds whose answer is a
 * function of the request (the seed is part of it): a request is its own
 * key, and nothing is retried.
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

/** The portfolios, the ratings and the wrong-way scenarios the page offers. */
export function useXvaPortfolios() {
	return useQuery<XvaPortfolios, ApiError>({
		queryKey: queryKeys.xva.portfolios(),
		staleTime: Infinity,
		queryFn: ({ signal }) =>
			call<XvaPortfolios>(signal, (s) =>
				api.GET("/api/xva/portfolios", { signal: s }),
			),
	});
}

/** Exposure, adjustments, margin and capital of a netting set. */
export function useXvaNettingSet(request: XvaRequest) {
	return useQuery<XvaResponse, ApiError>({
		queryKey: queryKeys.xva.nettingSet(request),
		staleTime: STALE.rates,
		retry: false,
		placeholderData: keepPreviousData,
		queryFn: ({ signal }) =>
			call<XvaResponse>(signal, (s) =>
				api.POST("/api/xva/netting-set", { body: request, signal: s }),
			),
	});
}

/**
 * The sensitivities of the adjustments to every market quote. Asked for on
 * demand (`enabled`): it is a second simulation, and not every portfolio has
 * one.
 */
export function useXvaSensitivities(
	request: XvaSensitivitiesRequest,
	enabled: boolean,
) {
	return useQuery<XvaSensitivities, ApiError>({
		queryKey: queryKeys.xva.sensitivities(request),
		enabled,
		staleTime: STALE.rates,
		retry: false,
		queryFn: ({ signal }) =>
			call<XvaSensitivities>(signal, (s) =>
				api.POST("/api/xva/sensitivities", { body: request, signal: s }),
			),
	});
}
