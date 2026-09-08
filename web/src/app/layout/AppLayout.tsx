import { Suspense } from "react";
import { Outlet } from "@tanstack/react-router";
import { ChartSkeleton } from "@/shared/ui/states";
import { Nav } from "./Nav";
import { Footer } from "./Footer";

/**
 * The shell (WP 03). It renders children and loads nothing but /health.
 * Full-width for tables and surfaces; a reading max-width is applied per page
 * to prose only.
 */
export function AppLayout() {
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

function PageFallback() {
	return (
		<div className="mx-auto flex max-w-6xl flex-col gap-4">
			<div className="h-8 w-48 animate-pulse rounded-sm bg-surface-raised" />
			<ChartSkeleton />
		</div>
	);
}
