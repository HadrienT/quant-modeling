import {
	type UseQueryResult,
	keepPreviousData,
	useQuery,
} from "@tanstack/react-query";
import { LONG_TIMEOUT_MS, api, signalWithTimeout } from "./client";
import { ApiError } from "./errors";
import { usePricing } from "./pricing";
import { queryKeys } from "./queryKeys";
import type {
	RatesCurveQuotes,
	RatesCurvesResponse,
	RatesQuoteSet,
	RatesQuoteSetId,
	SwapPricingRequest,
	SwapPricingResponse,
	SwaptionPricingRequest,
	SwaptionPricingResponse,
} from "./types";

/**
 * Rates derivatives (api/app/rates_derivatives.py): the quote sets, the
 * curves built from a set of quotes, and the swap and swaption priced on
 * them. A price is a function of the quotes its request carries, so a request
 * is its own key.
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

/**
 * A quote set: `usd-sofr`, built from traded swaps and swaptions (DTCC, stored
 * by data-ingest) — a 503 says what is missing from the store — or
 * `eur-illustrative`, the manual multi-curve example.
 */
export function useRatesQuoteSet(id: RatesQuoteSetId, enabled = true) {
	return useQuery<RatesQuoteSet, ApiError>({
		queryKey: queryKeys.rates.quotes(id),
		enabled,
		staleTime: id === "usd-sofr" ? 60 * 60 * 1000 : Infinity,
		retry: false,
		queryFn: ({ signal }) =>
			call<RatesQuoteSet>(signal, (s) =>
				api.GET("/api/rates/quotes/{set_id}", {
					params: { path: { set_id: id } },
					signal: s,
				}),
			),
	});
}

/** The OIS and index curves bootstrapped from a set of quotes. */
export function useRatesCurves(quotes: RatesCurveQuotes | undefined) {
	return useQuery<RatesCurvesResponse, ApiError>({
		queryKey: queryKeys.rates.curves(quotes),
		enabled: !!quotes,
		staleTime: Infinity,
		retry: false,
		placeholderData: keepPreviousData,
		queryFn: ({ signal }) =>
			call<RatesCurvesResponse>(signal, (s) =>
				api.POST("/api/rates/curves", { body: quotes!, signal: s }),
			),
	});
}

type Timed<T> = T & { round_trip_ms: number };

/** A swap priced on the quotes of its request (a recorded valuation). */
export function useSwapPricing(body: SwapPricingRequest | undefined) {
	return usePricing({
		endpoint: "/price/rates/swap",
		body,
		enabled: !!body,
	}) as UseQueryResult<Timed<SwapPricingResponse>, ApiError>;
}

/** A swaption priced on the quotes of its request (a recorded valuation). */
export function useSwaptionPricing(body: SwaptionPricingRequest | undefined) {
	return usePricing({
		endpoint: "/price/rates/swaption",
		body,
		enabled: !!body,
	}) as UseQueryResult<Timed<SwaptionPricingResponse>, ApiError>;
}
