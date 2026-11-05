import { useQuery } from "@tanstack/react-query";
import { LONG_TIMEOUT_MS, api, signalWithTimeout } from "./client";
import { ApiError } from "./errors";
import { queryKeys } from "./queryKeys";
import type { paths } from "./schema.gen";
import type { ConvergenceResponse, PricingResponse } from "./types";

/** Every pricing route: a POST under /price/** returning a pricing response
 *  (GET /price/devices is under /price/ too, and is not one; nor is the
 *  convergence study a Monte-Carlo route has next to it). */
export type PricingPath = {
	[K in keyof paths]: K extends `/price/${string}/convergence`
		? never
		: K extends `/price/${string}`
			? [NonNullable<paths[K]["post"]>] extends [never]
				? never
				: K
			: never;
}[keyof paths];

/** The convergence study next to a Monte-Carlo pricing route. */
export type ConvergencePath = Extract<
	keyof paths,
	`/price/${string}/convergence`
>;

/**
 * A pricing response plus the round trip the browser measured (request sent
 * → response parsed). The server's own compute time is `compute_ms`; the
 * difference is network, queueing and (de)serialisation.
 */
export type PricingResult = PricingResponse & { round_trip_ms: number };

/** Where a Monte-Carlo pricing can run (blueprint WP 19 §8). */
export type ComputeDevice = "cpu" | "gpu" | "auto";

/** Where a pricing ran: "CPU", "GPU" or "2 GPUs" (blueprint WP 19, lot G4). */
export function deviceLabel(r: { device?: string; gpus?: number }): string {
	if (r.device !== "gpu") return "CPU";
	return (r.gpus ?? 1) > 1 ? `${r.gpus} GPUs` : "GPU";
}

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

/**
 * The engine's standard error measured at N/32 ... N/2 paths, each point its
 * own pricing (api/app/convergence.py); the point at N is the pricing itself. `null` for a pricing route that has
 * no Monte Carlo (no `<route>/convergence`).
 */
export function convergencePath(endpoint: PricingPath | null) {
	if (endpoint === null) return null;
	const path = `${endpoint}/convergence`;
	return (CONVERGENCE_PATHS as readonly string[]).includes(path)
		? (path as ConvergencePath)
		: null;
}

/** Kept in step with schema.gen.ts by the type below: a route added or
 *  removed on the API side fails the typecheck until it is listed here. */
const CONVERGENCE_PATHS = [
	"/price/option/vanilla/convergence",
	"/price/option/asian/convergence",
	"/price/option/dated-asian/convergence",
	"/price/scripted/convergence",
	"/price/scripted-product/convergence",
	"/price/option/barrier/convergence",
	"/price/option/lookback/convergence",
	"/price/option/basket/convergence",
	"/price/structured/autocall/convergence",
	"/price/structured/mountain/convergence",
	"/price/volatility/variance-swap/convergence",
	"/price/volatility/volatility-swap/convergence",
	"/price/volatility/dispersion-swap/convergence",
	"/price/option/quanto/convergence",
	"/price/option/rainbow/convergence",
] as const satisfies readonly ConvergencePath[];
type _AllListed = ConvergencePath extends (typeof CONVERGENCE_PATHS)[number]
	? true
	: never;
const _allListed: _AllListed = true;
void _allListed;

export function useConvergence(args: {
	path: ConvergencePath | null;
	body: unknown;
	enabled?: boolean;
}) {
	const { path, body, enabled = true } = args;
	// N/32 ... N/2 only: the point at N is the caller's own pricing.
	return useQuery<ConvergenceResponse, ApiError>({
		queryKey: queryKeys.pricing.convergence(path ?? "", body),
		enabled: enabled && path !== null,
		staleTime: 0,
		retry: false,
		queryFn: async ({ signal }) => {
			const s = signalWithTimeout(signal, LONG_TIMEOUT_MS);
			try {
				const { data, error } = await api.POST(path as ConvergencePath, {
					body: body as never,
					signal: s,
				});
				if (error !== undefined) throw ApiError.from(error);
				return data as ConvergenceResponse;
			} finally {
				s.cleanup();
			}
		},
	});
}
