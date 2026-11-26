/** The market part of a scripting request: one ticker, or the typed inputs. */
export function singleMarket(
	market: boolean,
	ticker: string,
	spot: string,
	divPct: string,
	volPct: string,
) {
	return market
		? { ticker }
		: {
				spot: Number(spot),
				dividend: Number(divPct) / 100,
				vol: Number(volPct) / 100,
			};
}

/** The rate, sent only when typed: empty is the market's (WP 31 L6). */
export const rateRequest = (ratePct: string) =>
	ratePct.trim() === "" ? {} : { rate: Number(ratePct) / 100 };
