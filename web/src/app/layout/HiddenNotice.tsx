import { Link } from "@tanstack/react-router";
import { EyeOff } from "lucide-react";
import { Button } from "@/shared/ui";
import { EmptyState } from "@/shared/ui/states";

/** What a hidden page shows instead (visitor) or above itself (admin). */
export default function HiddenNotice({ kind }: { kind: "visitor" | "admin" }) {
	if (kind === "admin")
		return (
			<div
				role="status"
				className="mx-auto mb-4 flex max-w-6xl items-center gap-2 rounded-sm border border-hairline bg-surface-raised px-3 py-2 text-xs text-ink-secondary"
			>
				<EyeOff className="size-3.5 shrink-0" aria-hidden="true" />
				<span>
					Hidden from visitors: only administrators see this page.{" "}
					<Link to="/admin" className="underline underline-offset-2">
						Admin panel
					</Link>
				</span>
			</div>
		);
	return (
		<div className="mx-auto max-w-lg py-12">
			<EmptyState
				kind="no-results"
				title="Page not available"
				description="This page is not available at the moment."
				action={
					<Button asChild size="sm" variant="secondary">
						<Link to="/">Go to the home page</Link>
					</Button>
				}
			/>
		</div>
	);
}
