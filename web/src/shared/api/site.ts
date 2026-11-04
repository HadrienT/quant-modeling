import { useMutation, useQuery, useQueryClient } from "@tanstack/react-query";
import { api } from "./client";
import { ApiError } from "./errors";
import { queryKeys } from "./queryKeys";
import type { components } from "./schema.gen";

/**
 * Which pages visitors see (api/app/site_pages.py). Read by the shell on every
 * visit; changed from the admin panel.
 */

export type AdminSitePages = components["schemas"]["SitePages"];

/** The pages hidden from visitors. */
export function useHiddenPages() {
	return useQuery<string[], ApiError>({
		queryKey: queryKeys.site.pages(),
		queryFn: async ({ signal }) => {
			const { data, error } = await api.GET("/api/site/pages", { signal });
			if (error !== undefined) throw ApiError.from(error);
			return data?.hidden ?? [];
		},
		staleTime: 60_000,
		retry: 1,
	});
}

/** The same list, with who changed it last and when (administrators only). */
export function useAdminPages(enabled: boolean) {
	return useQuery<AdminSitePages, ApiError>({
		queryKey: queryKeys.site.adminPages(),
		queryFn: async ({ signal }) => {
			const { data, error } = await api.GET("/api/admin/pages", { signal });
			if (error !== undefined) throw ApiError.from(error);
			return data as AdminSitePages;
		},
		enabled,
		retry: false,
	});
}

/** Replaces the hidden pages; the navigation follows at once. */
export function useSetHiddenPages() {
	const qc = useQueryClient();
	return useMutation<AdminSitePages, ApiError, string[]>({
		mutationFn: async (hidden) => {
			const { data, error } = await api.PUT("/api/admin/pages", {
				body: { hidden },
			});
			if (error !== undefined) throw ApiError.from(error);
			return data as AdminSitePages;
		},
		onMutate: (hidden) => {
			qc.setQueryData(queryKeys.site.pages(), hidden);
		},
		onSuccess: (pages) => {
			qc.setQueryData(queryKeys.site.pages(), pages.hidden);
			qc.setQueryData(queryKeys.site.adminPages(), pages);
		},
		onError: () => {
			void qc.invalidateQueries({ queryKey: queryKeys.site.pages() });
		},
	});
}
