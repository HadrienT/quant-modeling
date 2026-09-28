import { Link } from "@tanstack/react-router";
import { Info } from "lucide-react";
import { cn } from "./cn";

/**
 * The financial disclaimer, one line, on the pages whose numbers look like
 * something to act on (pricing, portfolio, backtest). The full text is /terms.
 */
export function Disclaimer({ className }: { className?: string }) {
	return (
		<p
			className={cn(
				"flex items-start gap-1.5 text-2xs text-ink-muted",
				className,
			)}
		>
			<Info className="mt-px size-3 shrink-0" aria-hidden="true" />
			<span>
				Model output for education and research, not investment advice.{" "}
				<Link to="/terms" className="underline-offset-2 hover:underline">
					Terms &amp; disclaimer
				</Link>
			</span>
		</p>
	);
}
