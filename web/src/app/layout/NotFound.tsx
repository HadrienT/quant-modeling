import { Link } from "@tanstack/react-router";
import { EmptyState } from "@/shared/ui/states";
import { useNavPages } from "@/shared/session";
import { Button } from "@/shared/ui";

export function NotFound() {
	const { isShown } = useNavPages();
	return (
		<div className="mx-auto max-w-lg py-12">
			<EmptyState
				kind="no-results"
				title="Page not found"
				description="That route does not exist."
				action={
					<Button asChild size="sm" variant="secondary">
						{isShown("/visualize") ? (
							<Link to="/visualize">Go to Strategies</Link>
						) : (
							<Link to="/">Go to the home page</Link>
						)}
					</Button>
				}
			/>
		</div>
	);
}
