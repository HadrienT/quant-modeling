import { type ReactNode, Suspense, lazy } from "react";
import { useRouterState } from "@tanstack/react-router";
import { NAV_PAGES } from "@/shared/config";
import { useNavPages } from "@/shared/session";

// Only a visit to a hidden page needs these: out of the shell's budget.
const HiddenNotice = lazy(() => import("./HiddenNotice"));

const GATED = new Set<string>(NAV_PAGES.map((p) => p.path));

/**
 * Hides from visitors the pages an administrator turned off in the admin
 * panel: a link or a bookmark to one lands on "not available". An
 * administrator still sees the page, under a banner that says so.
 */
export function PageGate({
	children,
	fallback,
}: {
	children: ReactNode;
	fallback: ReactNode;
}) {
	const path = useRouterState({ select: (s) => s.location.pathname });
	const { isHidden, isAdmin, settled } = useNavPages();

	if (!GATED.has(path)) return children;
	// Wait for the answer rather than flash a page about to disappear.
	if (!settled) return fallback;
	if (!isHidden(path)) return children;
	if (!isAdmin)
		return (
			<Suspense fallback={fallback}>
				<HiddenNotice kind="visitor" />
			</Suspense>
		);
	return (
		<>
			<Suspense fallback={null}>
				<HiddenNotice kind="admin" />
			</Suspense>
			{children}
		</>
	);
}
