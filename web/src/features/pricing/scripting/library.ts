import { SCRIPTED_PRODUCTS, type ScriptedProduct } from "@/shared/products";

/**
 * The product library of the scripting page: the script library of the API
 * (api/app/product_library/*.qms, generated into shared/products'
 * scripted.gen.json), whose products the pricing catalog also offers from a
 * term sheet. Here the script itself is loaded into the editor, with its
 * default terms assigned at the first event.
 */
export type LibraryProduct = ScriptedProduct;

export const CATEGORY_ORDER = [
	"Vanillas and digitals",
	"Barriers and touch options",
	"Asians, lookbacks and ladders",
	"Cliquets",
	"Structured notes",
	"Volatility",
	"Multi-asset",
] as const;

const rank = (c: string) =>
	(CATEGORY_ORDER as readonly string[]).indexOf(c) >>> 0;

export const SCRIPT_LIBRARY: LibraryProduct[] = [...SCRIPTED_PRODUCTS].sort(
	(a, b) => rank(a.category) - rank(b.category) || a.slug.localeCompare(b.slug),
);

export const DEFAULT_PRODUCT =
	SCRIPT_LIBRARY.find((p) => p.slug === "european-call") ?? SCRIPT_LIBRARY[0]!;
