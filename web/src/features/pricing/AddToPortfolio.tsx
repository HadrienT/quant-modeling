import { Link } from "@tanstack/react-router";
import { BriefcaseBusiness } from "lucide-react";
import { bookableScript, encodeParams } from "@/shared/products";
import { useNavPages } from "@/shared/session";

/**
 * Carries the product on screen to the portfolio's trade form (WP 28, lot
 * U2; issue 313, lot b3: the library's products), for those a portfolio
 * holds. Its terms and its underlying travel; the trade date, and the
 * script's dates with it, are set there, and every day's mark replaces the
 * market inputs with that day's market.
 */
export function AddToPortfolio({
	productKey,
	values,
}: {
	productKey: string;
	values: Record<string, unknown>;
}) {
	const { isShown } = useNavPages();
	if (!isShown("/portfolio") || !bookableScript(productKey)) return null;
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
