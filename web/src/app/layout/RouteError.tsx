import { useRouter } from "@tanstack/react-router";
import { ErrorState } from "@/shared/ui/states";

/** Per-route error boundary (WP 03 §5). A failing page shows this, not a blank screen. */
export function RouteError({ error }: { error: unknown }) {
	const router = useRouter();
	return (
		<div className="mx-auto max-w-2xl py-8">
			<ErrorState error={error} onRetry={() => router.invalidate()} />
		</div>
	);
}
