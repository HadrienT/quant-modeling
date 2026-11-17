import { useHiddenPages } from "@/shared/api";
import {
	NAV_GROUPS,
	NAV_PAGES,
	PAGE_HINTS,
	type NavPage,
} from "@/shared/config";
import { useMe } from "./session";

export type VisiblePage = NavPage & { hidden: boolean; hint: string };
export type VisibleGroup = {
	key: string;
	label: string;
	question: string;
	pages: VisiblePage[];
};

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
		hint: PAGE_HINTS[p.path],
		hidden: hidden.has(p.path),
	})).filter((p) => isAdmin || !p.hidden);
	// A space with no page left for this visitor does not show.
	const groups: VisibleGroup[] = NAV_GROUPS.map((g) => ({
		key: g.key,
		label: g.label,
		question: g.question,
		pages: g.pages.flatMap((path) => pages.filter((p) => p.path === path)),
	})).filter((g) => g.pages.length > 0);
	// Pages in no space (About): links of their own in the bar.
	const grouped = new Set(
		NAV_GROUPS.flatMap((g) => g.pages as readonly string[]),
	);
	const standalone = pages.filter((p) => !grouped.has(p.path));
	return {
		pages,
		groups,
		standalone,
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
