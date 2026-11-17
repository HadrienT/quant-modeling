import { Link } from "@tanstack/react-router";
import { BriefcaseBusiness } from "lucide-react";
import { encodeParams, portfolioDescriptor } from "@/shared/products";
import { useNavPages } from "@/shared/session";

/**
 * Carries the product on screen to the portfolio's trade form (WP 28, lot
 * U2), for the products a portfolio can hold. Only the terms travel: the
 * underlying, expiry and price are set there, and every day's mark replaces
 * the market inputs with that day's market.
 */
export function AddToPortfolio({
	productKey,
	values,
}: {
	productKey: string;
	values: Record<string, unknown>;
}) {
	const { isShown } = useNavPages();
	if (!isShown("/portfolio") || !portfolioDescriptor(productKey)) return null;
	return (
		<Link
			to="/portfolio"
			search={{ trade: encodeParams({ product: productKey, values }) }}
			title="Book this product in a portfolio: its terms fill the trade form"
			className="inline-flex items-center gap-1 text-2xs text-ink-muted transition-colors hover:text-accent"
		>
			<BriefcaseBusiness className="size-3.5" />
			Add to portfolio
		</Link>
	);
}
