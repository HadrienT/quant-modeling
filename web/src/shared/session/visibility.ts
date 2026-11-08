import { useHiddenPages } from "@/shared/api";
import { NAV_PAGES, type NavPage } from "@/shared/config";
import { useMe } from "./session";

export type VisiblePage = NavPage & { hidden: boolean };

/**
 * The navigation as this visitor sees it. A visitor never sees a page an
 * administrator hid; an administrator sees every page, the hidden ones
 * flagged. Should the list fail to load, every page is shown: hiding a page
 * is presentation, not access control.
 */
export function useNavPages() {
	const me = useMe();
	const hiddenQuery = useHiddenPages();
	const hidden = new Set(hiddenQuery.data ?? []);
	const isAdmin = me.data?.is_admin === true;
	const pages: VisiblePage[] = NAV_PAGES.map((p) => ({
		...p,
		hidden: hidden.has(p.path),
	})).filter((p) => isAdmin || !p.hidden);
	return {
		pages,
		isAdmin,
		isHidden: (path: string) => hidden.has(path),
		/** whether a link to this page should show to this visitor */
		isShown: (path: string) => isAdmin || !hidden.has(path),
		/**
		 * both the list and the session have answered (or failed) at least
		 * once. Not `!isPending`: a query that failed with no data goes back to
		 * pending on its next fetch, and every page mounting it refetches, so a
		 * gate waiting on `isPending` would hide the page, remount it, refetch,
		 * and loop.
		 */
		settled: hiddenQuery.isFetched && me.isFetched,
	};
}
