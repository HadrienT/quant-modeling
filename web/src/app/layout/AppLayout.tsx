import { Suspense, useEffect } from "react";
import { Outlet, useRouterState } from "@tanstack/react-router";
import { SITE } from "@/shared/config";
import { PAGE_TITLES } from "@/app/router";
import { ChartSkeleton } from "@/shared/ui/states";
import { Nav } from "./Nav";
import { Footer } from "./Footer";

/**
 * The shell (WP 03). It renders children and loads nothing but /health.
 * Full-width for tables and surfaces; a reading max-width is applied per page
 * to prose only.
 */
export function AppLayout() {
	usePageTitle();
	return (
		<div className="flex min-h-screen flex-col bg-canvas text-ink">
			<Nav />
			<main className="flex-1 px-4 py-5">
				<Suspense fallback={<PageFallback />}>
					<Outlet />
				</Suspense>
			</main>
			<Footer />
		</div>
	);
}

/** "Pricing · Quant Modeling": tabs, history and bookmarks tell pages apart. */
function usePageTitle() {
	const path = useRouterState({ select: (s) => s.location.pathname });
	useEffect(() => {
		const page = PAGE_TITLES[path];
		document.title = page
			? `${page} · ${SITE.name}`
			: `${SITE.name} — derivatives pricing`;
	}, [path]);
}

function PageFallback() {
	return (
		<div className="mx-auto flex max-w-6xl flex-col gap-4">
			<div className="h-8 w-48 animate-pulse rounded-sm bg-surface-raised" />
			<ChartSkeleton />
		</div>
	);
}
