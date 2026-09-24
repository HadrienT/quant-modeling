import { useQuery } from "@tanstack/react-query";
import { LONG_TIMEOUT_MS, api, signalWithTimeout } from "./client";
import { ApiError } from "./errors";
import { queryKeys } from "./queryKeys";
import type { paths } from "./schema.gen";
import type { PricingResponse } from "./types";

/** Every pricing route: a POST under /price/** returning a pricing response. */
export type PricingPath = {
	[K in keyof paths]: K extends `/price/${string}` ? K : never;
}[keyof paths];

/**
 * A pricing response plus the round trip the browser measured (request sent
 * → response parsed). The server's own compute time is `compute_ms`; the
 * difference is network, queueing and (de)serialisation.
 */
export type PricingResult = PricingResponse & { round_trip_ms: number };

/**
 * Price a single instrument. The concrete request/response types are enforced
 * per-endpoint by openapi-fetch; the workbench (WP 07) narrows them via the
 * product descriptor.
 */
export function usePricing<P extends PricingPath>(args: {
	endpoint: P | null;
	body: unknown;
	enabled?: boolean;
}) {
	const { endpoint, body, enabled = true } = args;
	return useQuery<PricingResult, ApiError>({
		queryKey: queryKeys.pricing.option(endpoint ?? "", body),
		enabled: enabled && Boolean(endpoint),
		staleTime: 0, // a price is recomputed whenever its inputs (the key) change
		retry: false,
		queryFn: async ({ signal }) => {
			const s = signalWithTimeout(signal, LONG_TIMEOUT_MS);
			const sent = performance.now();
			try {
				const { data, error } = await api.POST(endpoint as PricingPath, {
					body: body as never,
					signal: s,
				});
				if (error !== undefined) throw ApiError.from(error);
				return {
					...(data as PricingResponse),
					round_trip_ms: performance.now() - sent,
				};
			} finally {
				s.cleanup();
			}
		},
	});
}
