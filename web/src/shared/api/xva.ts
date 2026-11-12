import { keepPreviousData, useQuery } from "@tanstack/react-query";
import { api } from "./client";
import type { ApiError } from "./errors";
import { longCall as call } from "./longCall";
import { queryKeys } from "./queryKeys";
import { STALE } from "./queryClient";
import type {
	PortfolioXvaRequest,
	PortfolioXvaSensitivitiesRequest,
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
 * The same for one netting set of a portfolio's ledger (null: not asked
 * for — the run takes a few seconds and is started by the reader).
 */
export function usePortfolioXva(request: PortfolioXvaRequest | null) {
	return useQuery<XvaResponse, ApiError>({
		queryKey: queryKeys.xva.portfolio(request),
		enabled: request != null,
		staleTime: STALE.rates,
		retry: false,
		queryFn: ({ signal }) =>
			call<XvaResponse>(signal, (s) =>
				api.POST("/api/xva/portfolio", {
					body: request as never,
					signal: s,
				}),
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

/** The same for one netting set of a portfolio's ledger. */
export function usePortfolioXvaSensitivities(
	request: PortfolioXvaSensitivitiesRequest | null,
	enabled: boolean,
) {
	return useQuery<XvaSensitivities, ApiError>({
		queryKey: queryKeys.xva.portfolioSensitivities(request),
		enabled: enabled && request != null,
		staleTime: STALE.rates,
		retry: false,
		queryFn: ({ signal }) =>
			call<XvaSensitivities>(signal, (s) =>
				api.POST("/api/xva/portfolio/sensitivities", {
					body: request as never,
					signal: s,
				}),
			),
	});
}
