import {
	type RatesCurveQuotes,
	type RatesQuoteSetId,
	type SwapInput,
	type SwapPricingRequest,
	type SwaptionInput,
	type SwaptionPricingRequest,
	type SwaptionVolInput,
	useRatesQuoteSet,
} from "@/shared/api";

/** Everything a rates price is computed from, besides the contract. */
export type RatesQuotes = {
	curves: RatesCurveQuotes;
	swaption_vols: SwaptionVolInput[];
	/** Hull-White a, fixed by the user; null fits it. */
	hull_white_mean_reversion: number | null;
};

/**
 * The form values of a rates product (kept in the workbench URL): the quote
 * set, and only what the user changed from it. An absent `quotes` or
 * `contract` means "the set's own".
 */
export type RatesValues = {
	quote_set?: RatesQuoteSetId;
	quotes?: RatesQuotes;
	contract?: SwapInput | SwaptionInput;
};

/**
 * Resolves a rates product's form values against its quote set: the quotes
 * and the contract actually priced, and whether the quotes are still the
 * set's. Both columns of the workbench call it; the set is fetched once.
 */
export function useRatesInputs(kind: "swap" | "swaption", values: RatesValues) {
	const setId = values.quote_set ?? "usd-sofr";
	const set = useRatesQuoteSet(setId);
	const base = set.data;
	const quotes: RatesQuotes | undefined =
		values.quotes ??
		(base && {
			curves: base.curves,
			swaption_vols: base.swaption_vols,
			hull_white_mean_reversion: null,
		});
	const contract = values.contract ?? base?.[kind];
	return { setId, set, quotes, contract, edited: values.quotes != null };
}

export function swapBody(
	quotes: RatesQuotes,
	swap: SwapInput,
): SwapPricingRequest {
	return { curves: quotes.curves, swap };
}

export function swaptionBody(
	quotes: RatesQuotes,
	swaption: SwaptionInput,
): SwaptionPricingRequest {
	return {
		curves: quotes.curves,
		swaption_vols: quotes.swaption_vols,
		hull_white_mean_reversion: quotes.hull_white_mean_reversion,
		swaption,
	};
}
