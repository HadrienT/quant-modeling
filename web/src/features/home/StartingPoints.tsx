import { Link } from "@tanstack/react-router";
import { ArrowRight } from "lucide-react";
import { useNavPages } from "@/shared/session";

/**
 * Three ways in, each one click from a result (WP 28): the link carries the
 * page's state in its URL, so the visitor lands on a computed screen and
 * edits from there. Hidden pages are not offered, as everywhere else.
 */
const STARTS = [
	{
		key: "surface",
		step: "Market",
		title: "Read the SPY volatility surface",
		text: "Implied volatility from the option chain, cleaned, then turned into a local volatility surface.",
		link: { to: "/market", search: { tab: "vol", ticker: "SPY" } },
	},
	{
		key: "barrier",
		step: "Price",
		title: "Price a barrier option",
		text: "One product under analytic and Monte Carlo engines, with greeks and the convergence of each.",
		link: { to: "/price", search: { product: "barrier" } },
	},
	{
		key: "cva",
		step: "Risk",
		title: "CVA of a swap against a BBB counterparty",
		text: "Simulated exposure of a single swap, then the credit, funding and capital adjustments on it.",
		link: { to: "/xva", search: { portfolio: "single_swap", cpty: "BBB" } },
	},
] as const;

export function StartingPoints() {
	const { isShown } = useNavPages();
	const starts = STARTS.filter((s) => isShown(s.link.to));
	if (starts.length === 0) return null;
	return (
		<section className="flex flex-col gap-4">
			<h2 className="text-lg font-semibold text-ink">Start here</h2>
			<ol className="grid grid-cols-1 gap-3 md:grid-cols-3">
				{starts.map((s) => (
					<li key={s.key}>
						<Link
							{...s.link}
							className="group flex h-full flex-col gap-2 rounded-md border border-hairline bg-surface p-4 transition-colors hover:border-accent/60"
						>
							<span className="text-2xs font-medium tracking-wide text-accent uppercase">
								{s.step}
							</span>
							<span className="flex items-start gap-2 text-sm font-semibold text-ink">
								{s.title}
								<ArrowRight className="mt-0.5 ml-auto size-4 shrink-0 text-ink-muted transition-transform group-hover:translate-x-0.5 group-hover:text-accent" />
							</span>
							<span className="text-xs leading-relaxed text-ink-secondary">
								{s.text}
							</span>
						</Link>
					</li>
				))}
			</ol>
		</section>
	);
}
