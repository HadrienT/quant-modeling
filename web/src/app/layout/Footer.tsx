import { useHealth } from "@/shared/api";
import { getConfig } from "@/shared/config";
import { cn } from "@/shared/ui";

/** Build version, repo link, and the one thing that answers "why is nothing loading" — API health. */
export function Footer() {
	const health = useHealth();
	const sha = getConfig().commitSha;

	const state = health.isLoading ? "pending" : health.isError ? "down" : "up";

	return (
		<footer className="flex items-center gap-4 border-t border-hairline px-4 py-2 text-2xs text-ink-muted">
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
				href="https://github.com/HadrienT/quant-modeling"
				target="_blank"
				rel="noreferrer"
				className="hover:text-ink"
			>
				repository
			</a>
		</footer>
	);
}
