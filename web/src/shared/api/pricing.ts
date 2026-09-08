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
	return useQuery<PricingResponse, ApiError>({
		queryKey: queryKeys.pricing.option(endpoint ?? "", body),
		enabled: enabled && Boolean(endpoint),
		staleTime: 0, // a price is recomputed whenever its inputs (the key) change
		retry: false,
		queryFn: async ({ signal }) => {
			const s = signalWithTimeout(signal, LONG_TIMEOUT_MS);
			try {
				const { data, error } = await api.POST(endpoint as PricingPath, {
					body: body as never,
					signal: s,
				});
				if (error !== undefined) throw ApiError.from(error);
				return data as PricingResponse;
			} finally {
				s.cleanup();
			}
		},
	});
}
