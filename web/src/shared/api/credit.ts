import { keepPreviousData, useQuery } from "@tanstack/react-query";
import { api, signalWithTimeout } from "./client";
import { ApiError } from "./errors";
import { queryKeys } from "./queryKeys";
import { STALE } from "./queryClient";
import type {
	CreditSpreadsResponse,
	FundamentalsResponse,
	SpreadHistoryResponse,
	StructuralResponse,
} from "./types";

/**
 * Credit page (api/app/routers/credit.py): market credit spreads, company
 * statements from 10-K / 10-Q filings, and the structural (Merton) model.
 * Spreads publish daily and filings weekly, hence the rates staleness.
 */

async function get<T>(
	signal: AbortSignal | undefined,
	call: (s: AbortSignal) => Promise<{ data?: unknown; error?: unknown }>,
): Promise<T> {
	const s = signalWithTimeout(signal);
	try {
		const { data, error } = await call(s);
		if (error !== undefined) throw ApiError.from(error);
		return data as T;
	} finally {
		s.cleanup();
	}
}

export function useCreditSpreads(recovery: number) {
	return useQuery({
		queryKey: queryKeys.credit.spreads(recovery),
		staleTime: STALE.rates,
		placeholderData: keepPreviousData,
		queryFn: ({ signal }) =>
			get<CreditSpreadsResponse>(signal, (s) =>
				api.GET("/api/credit/spreads/overview", {
					params: { query: { recovery } },
					signal: s,
				}),
			),
	});
}

export function useSpreadHistory(seriesId: string | undefined, years = 3) {
	return useQuery({
		queryKey: queryKeys.credit.spreadHistory(seriesId ?? "", years),
		enabled: !!seriesId,
		staleTime: STALE.rates,
		placeholderData: keepPreviousData,
		queryFn: ({ signal }) =>
			get<SpreadHistoryResponse>(signal, (s) =>
				api.GET("/api/credit/spreads/history", {
					params: { query: { series_id: seriesId!, years } },
					signal: s,
				}),
			),
	});
}

export function useCreditCompanies() {
	return useQuery({
		queryKey: queryKeys.credit.companies(),
		staleTime: STALE.rates,
		queryFn: ({ signal }) =>
			get<{ companies: FundamentalsResponse["company"][] }>(signal, (s) =>
				api.GET("/api/credit/companies", { signal: s }),
			),
	});
}

export function useFundamentals(
	ticker: string | undefined,
	frequency: "annual" | "quarterly",
) {
	return useQuery({
		queryKey: queryKeys.credit.fundamentals(ticker ?? "", frequency),
		enabled: !!ticker,
		staleTime: STALE.rates,
		placeholderData: keepPreviousData,
		queryFn: ({ signal }) =>
			get<FundamentalsResponse>(signal, (s) =>
				api.GET("/api/credit/companies/{ticker}/fundamentals", {
					params: {
						path: { ticker: ticker! },
						query: { frequency, periods: frequency === "annual" ? 6 : 8 },
					},
					signal: s,
				}),
			),
	});
}

export function useStructural(ticker: string | undefined) {
	return useQuery({
		queryKey: queryKeys.credit.structural(ticker ?? ""),
		enabled: !!ticker,
		staleTime: STALE.rates,
		queryFn: ({ signal }) =>
			get<StructuralResponse>(signal, (s) =>
				api.GET("/api/credit/companies/{ticker}/structural", {
					params: { path: { ticker: ticker! } },
					signal: s,
				}),
			),
	});
}
