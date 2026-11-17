import { Link } from "@tanstack/react-router";
import { useMarketContext, useNavPages } from "@/shared/session";

const MARKETS = ["SP500", "CAC40", "DAX", "FTSE100", "NIKKEI225"] as const;
type Market = (typeof MARKETS)[number];

/**
 * The underlying the pages start on (WP 28, lot U4), in the bar: what Market,
 * Credit and Simulation will open on, and a way back to its market data.
 */
export function ContextChip() {
	const context = useMarketContext();
	const { isShown } = useNavPages();
	if (!context || !isShown("/market")) return null;
	const market = (MARKETS as readonly string[]).includes(context.market)
		? (context.market as Market)
		: undefined;
	return (
		<Link
			to="/market"
			search={{ tab: "prices", ticker: context.ticker, market }}
			title={`Pages open on ${context.ticker}: pick another underlying on Market`}
			className="hidden items-center gap-1.5 rounded-sm border border-hairline px-2 py-0.5 font-mono text-2xs text-ink-secondary transition-colors hover:border-accent/60 hover:text-ink sm:flex"
		>
			<span className="size-1.5 rounded-full bg-accent" aria-hidden="true" />
			<span className="sr-only">Current underlying: </span>
			{context.ticker}
		</Link>
	);
}
