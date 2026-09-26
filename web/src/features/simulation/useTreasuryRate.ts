import { useRatesOverview } from "@/shared/api";

/**
 * The USD risk-free rate for a maturity, read off the stored US Treasury
 * curve (FRED, via data-ingest — never a live source): the continuously
 * compounded zero rate the API bootstraps from the par yields, interpolated
 * linearly in maturity and held flat beyond the first and last pillars.
 * The calibration takes one flat rate, so this is the zero rate AT the
 * target maturity. null while loading or when the curve is unavailable.
 */
export function useTreasuryRate(ttm: number) {
	const q = useRatesOverview("USD");
	const gov = q.data?.government;
	const zero = gov?.zero ?? [];
	if (!gov || zero.length === 0 || !(ttm > 0)) return null;

	let rate = zero[0]!.rate;
	if (ttm >= zero[zero.length - 1]!.tenor) rate = zero[zero.length - 1]!.rate;
	else
		for (let i = 1; i < zero.length; i++) {
			const a = zero[i - 1]!;
			const b = zero[i]!;
			if (ttm <= b.tenor) {
				rate =
					ttm <= a.tenor
						? a.rate
						: a.rate +
							((b.rate - a.rate) * (ttm - a.tenor)) / (b.tenor - a.tenor);
				break;
			}
		}
	return { rate, asOf: gov.as_of, ttm };
}
