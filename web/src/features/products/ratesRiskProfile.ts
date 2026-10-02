import {
	type RatesCurveQuotes,
	type RatesQuoteSet,
	type SwaptionPricingResponse,
	usePricing,
	useRatesQuoteSet,
} from "@/shared/api";
import { type RiskProfile, type RiskRow, classify } from "./riskProfile";

const BP = 1e-4;

const shift = <R extends { rate: number }>(rows: R[] | undefined, d: number) =>
	(rows ?? []).map((r) => ({ ...r, rate: r.rate + d }));

/** The curve quotes with the OIS side and the index side each moved. */
function bumped(
	c: RatesCurveQuotes,
	ois: number,
	index: number,
): RatesCurveQuotes {
	return {
		...c,
		deposits: shift(c.deposits, ois),
		ois: shift(c.ois, ois),
		fras: shift(c.fras, index),
		swaps: shift(c.swaps, index),
	};
}

function body(
	kind: "swap" | "swaption",
	set: RatesQuoteSet,
	curves: RatesCurveQuotes,
	volBump: number,
	strike: number | null,
) {
	if (kind === "swap") return { curves, swap: set.swap };
	return {
		curves,
		swaption_vols: set.swaption_vols.map((v) => ({
			...v,
			normal_vol: v.normal_vol + volBump,
		})),
		// Struck at today's forward: an at-the-money strike that followed the
		// bumped curve would hide the rate sensitivity.
		swaption: { ...set.swaption, strike },
	};
}

/**
 * Long or short what, for a rates product: bump the quotes and reprice
 * through the product's own endpoint, on the illustrative EUR set (always
 * available, and the one with two curves) and the workbench's default
 * contract. Curves are rebuilt and Hull-White recalibrated at each bump; the
 * prices are closed-form or lattice, so the changes carry no sampling error.
 */
export function useRatesRiskProfile(kind: "swap" | "swaption") {
	const set = useRatesQuoteSet("eur-illustrative");
	const s = set.data;
	const endpoint =
		kind === "swap"
			? ("/price/rates/swap" as const)
			: ("/price/rates/swaption" as const);
	const base = usePricing({
		endpoint,
		body: s && body(kind, s, s.curves, 0, null),
		enabled: !!s,
	});
	// The forward of the base pricing fixes the swaption's strike.
	const forward =
		kind === "swaption"
			? (base.data as SwaptionPricingResponse | undefined)?.swaption.forward
			: null;
	const ready = !!s && forward !== undefined;
	const price = (ois: number, index: number, vol: number) => ({
		endpoint,
		body: ready
			? body(kind, s, bumped(s.curves, ois, index), vol, forward)
			: null,
		enabled: ready,
	});
	const atStrike = usePricing(price(0, 0, 0));
	const parallel = usePricing(price(BP, BP, 0));
	const basis = usePricing(price(0, BP, 0));
	const vega = usePricing({
		...price(0, 0, BP),
		enabled: ready && kind === "swaption",
	});

	const all = [set, base, atStrike, parallel, basis, vega];
	const b0 = atStrike.data;
	const rows: RiskRow[] = [];
	const row = (factor: string, bump: string, npv: number | undefined) => {
		if (!b0 || npv === undefined) return;
		const change = npv - b0.npv;
		rows.push({
			factor,
			bump,
			change,
			std_error: 0,
			position: classify(change, 0, b0.npv),
		});
	};
	row(
		"Interest rates (parallel)",
		"+1 bp on every curve quote",
		parallel.data?.npv,
	);
	row(
		"Index basis over OIS",
		"+1 bp on the index quotes only (FRAs, index swaps)",
		basis.data?.npv,
	);
	if (kind === "swaption")
		row(
			"Volatility (vega)",
			"+1 bp of normal vol on every swaption quote",
			vega.data?.npv,
		);
	const data: RiskProfile | undefined =
		b0 && rows.length > 0
			? {
					rows,
					reference:
						"the illustrative EUR quotes and the pricing page's default contract" +
						(kind === "swaption"
							? ", struck at today's forward swap rate"
							: ""),
					method:
						"Bump the quotes and reprice through the product's own pricer, curves rebuilt each time; the prices are closed-form, so the changes carry no sampling error.",
				}
			: undefined;
	return {
		isLoading: all.some((q) => q.isLoading),
		error: all.find((q) => q.error)?.error ?? null,
		data,
	};
}
