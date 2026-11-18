import {
	type SwapInput,
	type SwaptionInput,
	useRatesQuoteSet,
} from "@/shared/api";
import type { ScriptedProduct } from "@/shared/products";
import { ErrorState, MetricRowSkeleton } from "@/shared/ui/states";
import { RatesResults } from "./RatesResults";

type Contract =
	| { kind: "swap"; tenor: number }
	| {
			kind: "swaption";
			expiry: number;
			tenor: number;
			exercise: "european" | "bermudan";
	  };

/**
 * What each SOFR script of the library is, on the workbench's regular grid
 * (api/app/product_library: annual periods, the first date a week after the
 * valuation date): the swap is five years long, each swaption is one year
 * into a five-year swap, the Bermudan exercisable every year.
 */
const CONTRACTS: Record<string, Contract> = {
	"interest-rate-swap": { kind: "swap", tenor: 5 },
	"european-swaption": {
		kind: "swaption",
		expiry: 1,
		tenor: 5,
		exercise: "european",
	},
	"bermudan-swaption": {
		kind: "swaption",
		expiry: 1,
		tenor: 5,
		exercise: "bermudan",
	},
};

/**
 * The diagnostics of a swap or swaption script (issue 313, lot b2b): the
 * same trade priced by the rates engines on the same SOFR quotes and the
 * same Hull-White calibration. They show the curve, the calibration fit
 * quote by quote, the cash flows, the swaption under every market model and
 * the Bermudan by backward induction on the lattice.
 */
export function RatesScriptDiagnostics({
	product,
	values,
}: {
	product: ScriptedProduct;
	values: Record<string, unknown>;
}) {
	const c = CONTRACTS[product.slug];
	const set = useRatesQuoteSet("usd-sofr", !!c);
	if (!c) return null;
	if (set.error)
		return <ErrorState error={set.error} onRetry={() => set.refetch()} />;
	if (!set.data) return <MetricRowSkeleton />;
	const side = Number(values.side ?? 1);
	const notional = Number(values.notional) * Math.abs(side);
	const payer = side > 0;
	const contract: SwapInput | SwaptionInput =
		c.kind === "swap"
			? {
					...set.data.swap,
					start: 0,
					tenor: c.tenor,
					fixed_rate: Number(values.fixed_rate) / 100,
					notional,
					payer,
				}
			: {
					...set.data.swaption,
					expiry: c.expiry,
					tenor: c.tenor,
					strike: Number(values.strike) / 100,
					notional,
					payer,
					exercise: c.exercise,
					model: "hull_white",
				};
	return (
		<section
			aria-label="The same trade on the rates engines"
			className="flex flex-col gap-3"
		>
			<h3 className="text-sm font-semibold text-ink">
				The same trade on the rates engines
			</h3>
			<p className="text-2xs text-ink-muted">
				Same SOFR quotes, same Hull-White calibration. The grid is different: it
				starts today and counts whole years. The script starts a week later and
				counts actual days, so the two values differ by that much and no more.
				{c.kind === "swaption" &&
					" The Bermudan here comes from the lattice (backward induction). The script's comes from Longstaff-Schwartz, a lower bound."}
			</p>
			<RatesResults
				kind={c.kind}
				values={{ quote_set: "usd-sofr", contract }}
			/>
		</section>
	);
}
