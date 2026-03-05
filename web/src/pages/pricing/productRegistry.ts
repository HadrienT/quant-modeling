/**
 * Product registry – single place to enable / disable every product
 * shown on the pricing page.
 *
 * Set `enabled: false` for any product you haven't vetted yet.
 * Disabled products still appear in the UI but are greyed-out and
 * cannot be selected.
 */

import type {
	CategoryType,
	CommodityProductType,
	ExoticProductType,
	FXProductType,
	InstrumentType,
	ModelType,
	StructuredProductType,
	VolProductType,
} from "./types";

/* ── helpers ─────────────────────────────────────── */

export type ProductEntry<K extends string = string> = {
	key: K;
	label: string;
	enabled: boolean;
};

export type CategoryEntry = ProductEntry<CategoryType>;

/* ── categories ──────────────────────────────────── */

export const CATEGORIES: CategoryEntry[] = [
	{ key: "vanilla", label: "Vanilla", enabled: true },
	{ key: "exotics", label: "Exotics", enabled: true },
	{ key: "fixed-income", label: "Fixed Income", enabled: true },
	{ key: "structured", label: "Structured", enabled: false },
	{ key: "volatility", label: "Volatility", enabled: false },
	{ key: "fx", label: "FX", enabled: false },
	{ key: "commodity", label: "Commodity", enabled: false },
];

/* ── models ──────────────────────────────────────── */

export const MODELS: ProductEntry<ModelType>[] = [
	{ key: "black-scholes", label: "Black-Scholes (flat vol)", enabled: true },
	{ key: "dupire-local-vol", label: "Dupire Local Vol", enabled: false },
];

/* ── vanilla instruments ─────────────────────────── */

export const VANILLA_INSTRUMENTS: ProductEntry<InstrumentType>[] = [
	{ key: "option", label: "Option", enabled: true },
	{ key: "future", label: "Future", enabled: true },
];

/* ── exotic products ─────────────────────────────── */

export const EXOTIC_PRODUCTS: ProductEntry<ExoticProductType>[] = [
	{ key: "barrier", label: "Barrier", enabled: true },
	{ key: "digital", label: "Digital", enabled: true },
	{ key: "lookback", label: "Lookback", enabled: true },
	{ key: "basket", label: "Basket", enabled: true },
	{ key: "rainbow", label: "Rainbow", enabled: true },
];

/* ── structured products ─────────────────────────── */

export const STRUCTURED_PRODUCTS: ProductEntry<StructuredProductType>[] = [
	{ key: "autocall", label: "Autocall", enabled: true },
	{ key: "mountain", label: "Mountain / Himalaya", enabled: true },
];

/* ── volatility products ─────────────────────────── */

export const VOL_PRODUCTS: ProductEntry<VolProductType>[] = [
	{ key: "variance-swap", label: "Variance Swap", enabled: true },
	{ key: "volatility-swap", label: "Volatility Swap", enabled: true },
	{ key: "dispersion-swap", label: "Dispersion Swap", enabled: true },
];

/* ── FX products ─────────────────────────────────── */

export const FX_PRODUCTS: ProductEntry<FXProductType>[] = [
	{ key: "fx-forward", label: "FX Forward", enabled: true },
	{ key: "fx-option", label: "FX Option (Garman–Kohlhagen)", enabled: true },
];

/* ── commodity products ──────────────────────────── */

export const COMMODITY_PRODUCTS: ProductEntry<CommodityProductType>[] = [
	{ key: "commodity-forward", label: "Commodity Forward", enabled: true },
	{ key: "commodity-option", label: "Commodity Option (Black '76)", enabled: true },
];

/* ── utility: render a <select> that respects enabled flags ── */

/**
 * Return the first enabled key from a list, or the first key if none
 * are enabled (fallback).
 */
export function firstEnabled<K extends string>(list: ProductEntry<K>[]): K {
	return (list.find((p) => p.enabled) ?? list[0]).key;
}

/**
 * Returns true when the given key is enabled in its list.
 */
export function isEnabled<K extends string>(list: ProductEntry<K>[], key: K): boolean {
	const entry = list.find((p) => p.key === key);
	return entry?.enabled ?? false;
}
