import { keepPreviousData, useQuery } from "@tanstack/react-query";
import { LONG_TIMEOUT_MS, api, signalWithTimeout } from "./client";
import { ApiError } from "./errors";
import { queryKeys } from "./queryKeys";
import type {
	RatesAnalysisRequest,
	RatesAnalysisResponse,
	RatesExampleResponse,
} from "./types";

/**
 * Rates derivatives page (api/app/routers/rates_derivatives.py): the
 * illustrative quotes it starts from, and the analysis of whatever quotes the
 * user has entered — computed, never stored, so a request is its own key.
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

export function useRatesExample() {
	return useQuery({
		queryKey: queryKeys.rates.example(),
		staleTime: Infinity,
		queryFn: ({ signal }) =>
			call<RatesExampleResponse>(signal, (s) =>
				api.GET("/api/rates/example", { signal: s }),
			),
	});
}

export function useRatesAnalysis(request: RatesAnalysisRequest | undefined) {
	return useQuery({
		queryKey: queryKeys.rates.analysis(request),
		enabled: !!request,
		staleTime: Infinity,
		retry: false,
		placeholderData: keepPreviousData,
		queryFn: ({ signal }) =>
			call<RatesAnalysisResponse>(signal, (s) =>
				api.POST("/api/rates/analyse", { body: request!, signal: s }),
			),
	});
}
