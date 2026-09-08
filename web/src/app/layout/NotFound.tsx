import { Link } from "@tanstack/react-router";
import { EmptyState } from "@/shared/ui/states";
import { Button } from "@/shared/ui";

export function NotFound() {
	return (
		<div className="mx-auto max-w-lg py-12">
			<EmptyState
				kind="no-results"
				title="Page not found"
				description="That route does not exist."
				action={
					<Button asChild size="sm" variant="secondary">
						<Link to="/visualize">Go to Strategies</Link>
					</Button>
				}
			/>
		</div>
	);
}
