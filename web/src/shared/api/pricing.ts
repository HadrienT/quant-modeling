import { useQuery } from "@tanstack/react-query";
import { LONG_TIMEOUT_MS, api, signalWithTimeout } from "./client";
import { ApiError } from "./errors";
import { queryKeys } from "./queryKeys";
import type { paths } from "./schema.gen";
import type { PricingResponse } from "./types";

/** Every pricing route: a POST under /price/** returning a pricing response
 *  (GET /price/devices is under /price/ too, and is not one). */
export type PricingPath = {
	[K in keyof paths]: K extends `/price/${string}`
		? [NonNullable<paths[K]["post"]>] extends [never]
			? never
			: K
		: never;
}[keyof paths];

/**
 * A pricing response plus the round trip the browser measured (request sent
 * → response parsed). The server's own compute time is `compute_ms`; the
 * difference is network, queueing and (de)serialisation.
 */
export type PricingResult = PricingResponse & { round_trip_ms: number };

/** Where a Monte-Carlo pricing can run (blueprint WP 19 §8). */
export type ComputeDevice = "cpu" | "gpu" | "auto";

/** The server's CPU model and usable GPUs; fixed for the process's life. */
export function useComputeDevices() {
	return useQuery({
		queryKey: queryKeys.pricing.devices(),
		queryFn: async ({ signal }) => {
			const s = signalWithTimeout(signal, 10_000);
			try {
				const { data, error } = await api.GET("/price/devices", { signal: s });
				if (error !== undefined) throw ApiError.from(error);
				return data;
			} finally {
				s.cleanup();
			}
		},
		staleTime: Number.POSITIVE_INFINITY,
		retry: false,
	});
}

/** One pricing call outside the query cache (the CPU-vs-GPU race). */
export async function priceOnce(
	endpoint: PricingPath,
	body: unknown,
): Promise<PricingResult> {
	const s = signalWithTimeout(undefined, LONG_TIMEOUT_MS);
	const sent = performance.now();
	try {
		const { data, error } = await api.POST(endpoint, {
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
}

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
