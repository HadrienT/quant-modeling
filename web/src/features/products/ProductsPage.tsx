import { useNavigate, useSearch } from "@tanstack/react-router";
import { CATALOG, CATALOG_BY_KEY, ProductPicker } from "@/shared/products";
import { ProductReference } from "./ProductReference";

/**
 * Products reference page. The same categorised catalog as the pricing
 * workbench on the left (disabled products stay clickable here — you can read
 * about one you cannot price), a full sourced reference sheet on the right.
 * The selected product lives in the URL, so a sheet is a shareable link.
 */
export default function ProductsPage() {
	const { product } = useSearch({ from: "/products" });
	const navigate = useNavigate({ from: "/products" });

	const key =
		product && CATALOG_BY_KEY.has(product) ? product : CATALOG[0]!.key;
	const descriptor = CATALOG_BY_KEY.get(key)!;

	return (
		<div className="mx-auto grid max-w-[1400px] gap-8 lg:grid-cols-[240px_1fr]">
			<aside className="lg:sticky lg:top-16 lg:max-h-[calc(100vh-5rem)] lg:self-start lg:overflow-y-auto lg:pb-6">
				<ProductPicker
					selected={key}
					onSelect={(k) => navigate({ search: { product: k } })}
					allowDisabled
				/>
			</aside>
			<ProductReference descriptor={descriptor} />
		</div>
	);
}
