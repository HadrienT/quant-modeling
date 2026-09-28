import { Link } from "@tanstack/react-router";
import { useHealth } from "@/shared/api";
import { SITE, getConfig } from "@/shared/config";
import { cn } from "@/shared/ui";

/**
 * Build version, repo link, and the one thing that answers "why is nothing
 * loading" — API health. The legal pages and the one-line disclaimer are here
 * because the footer is on every page.
 */
export function Footer() {
	const health = useHealth();
	const sha = getConfig().commitSha;

	const state = health.isLoading ? "pending" : health.isError ? "down" : "up";

	return (
		<footer className="flex flex-wrap items-center gap-x-4 gap-y-1 border-t border-hairline px-4 py-2 text-2xs text-ink-muted">
			<span className="flex items-center gap-1.5">
				<span
					className={cn(
						"size-1.5 rounded-full",
						state === "up" && "bg-good",
						state === "down" && "bg-critical",
						state === "pending" && "bg-ink-muted",
					)}
					aria-hidden="true"
				/>
				API {state === "up" ? "online" : state === "down" ? "unreachable" : "…"}
				{health.data?.version && state === "up"
					? ` · ${health.data.version}`
					: ""}
			</span>
			<span className="font-mono">build {sha}</span>
			<a
				href={SITE.repository}
				target="_blank"
				rel="noreferrer"
				className="hover:text-ink"
			>
				repository
			</a>
			<span className="basis-full sm:ml-auto sm:basis-auto">
				Educational tool, not investment advice.
			</span>
			<nav aria-label="Legal" className="flex gap-3">
				<Link to="/legal" className="hover:text-ink">
					Legal notice
				</Link>
				<Link to="/privacy" className="hover:text-ink">
					Privacy
				</Link>
				<Link to="/terms" className="hover:text-ink">
					Terms
				</Link>
			</nav>
		</footer>
	);
}
