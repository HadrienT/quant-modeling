import { QueryClient } from "@tanstack/react-query";
import { ApiError } from "./errors";

/**
 * staleTime is chosen per data family (blueprint WP 02 §3, pitfalls):
 *   - market surfaces: minutes (expensive server-side; freshness shown in UI)
 *   - price history:   minutes
 *   - pricing:         on demand (staleTime 0 — a price is always recomputed
 *                      when its inputs change, and inputs are the query key)
 *   - portfolio:       invalidated explicitly after a mutation
 */
export const STALE = {
	surface: 5 * 60_000,
	history: 5 * 60_000,
	rates: 10 * 60_000,
	pricing: 0,
	portfolio: 30_000,
} as const;

export function makeQueryClient(): QueryClient {
	return new QueryClient({
		defaultOptions: {
			queries: {
				// Market data is costly — do not refetch just because a window regained focus.
				refetchOnWindowFocus: false,
				retry: (failureCount, error) => {
					if (error instanceof ApiError && !error.retriable) return false;
					return failureCount < 2;
				},
				staleTime: 60_000,
			},
			mutations: {
				retry: false,
			},
		},
	});
}
