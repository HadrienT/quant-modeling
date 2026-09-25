import { useQuery } from "@tanstack/react-query";
import { api, ApiError, usePricing } from "@/shared/api";
import type { ProductDescriptor } from "@/shared/products";

export type RiskRow = {
	factor: string;
	bump: string;
	change: number;
	std_error: number;
	position: "long" | "short" | "not significant" | "negligible";
};

export type RiskProfile = {
	rows: RiskRow[];
	reference: string;
	method: string;
};

/** Same rules as api/app/risk_profile.py: a change within two standard
 * errors is not significant, one below 0.01 % of the price negligible. */
export function classify(
	change: number,
	se: number,
	price: number,
): RiskRow["position"] {
	if (Math.abs(change) <= 2 * se) return "not significant";
	if (Math.abs(change) < 1e-4 * Math.abs(price)) return "negligible";
	return change > 0 ? "long" : "short";
}

/** A library product: computed by the API (paired errors, smile factors). */
export function useScriptedRiskProfile(slug: string | undefined) {
	return useQuery({
		queryKey: ["risk-profile", slug],
		enabled: Boolean(slug),
		staleTime: Infinity,
		queryFn: async (): Promise<RiskProfile> => {
			const { data, error } = await api.POST("/products/risk-profile", {
				body: { product: slug!, terms: {} },
			});
			if (error !== undefined) throw ApiError.from(error);
			const ref = data.reference;
			return {
				rows: data.rows,
				reference: `spot ${ref.spot}, vol ${(ref.vol! * 100).toFixed(0)}%, rate ${(ref.rate! * 100).toFixed(0)}%, dividend ${(ref.dividend! * 100).toFixed(0)}%, correlation ${(ref.correlation! * 100).toFixed(0)}%`,
				method: data.method,
			};
		},
	});
}

type Bump = {
	factor: string;
	bump: string;
	field: string;
	apply: (x: number) => number;
};

const SPOT_UP: Bump = {
	factor: "",
	bump: "",
	field: "spot",
	apply: (x) => x * 1.01,
};
const SPOT_DOWN: Bump = {
	factor: "",
	bump: "",
	field: "spot",
	apply: (x) => x * 0.99,
};
const BUMPS: Bump[] = [
	{
		factor: "Volatility (vega)",
		bump: "+1 vol point",
		field: "vol",
		apply: (x) => x + 1,
	},
	{
		factor: "Interest rates (rho)",
		bump: "+50 bp",
		field: "rate",
		apply: (x) => x + 0.5,
	},
	{
		factor: "Dividends",
		bump: "+50 bp of yield",
		field: "dividend",
		apply: (x) => x + 0.5,
	},
	{
		factor: "Correlation",
		bump: "+10 points between every pair",
		field: "pairwise_correlation",
		apply: (x) => Math.min(x + 0.1, 0.999),
	},
	{
		factor: "Correlation (asset / FX)",
		bump: "+10 points",
		field: "correlation",
		apply: (x) => Math.min(x + 0.1, 0.999),
	},
];

function bumped(d: ProductDescriptor, b: Bump) {
	return b.field in d.defaults
		? { ...d.defaults, [b.field]: b.apply(Number(d.defaults[b.field])) }
		: null;
}

/** One reprice of a catalog product at its defaults, possibly bumped. */
function useProductPrice(
	d: ProductDescriptor,
	values: Record<string, unknown> | null,
) {
	const endpoint = d.enabled ? (d.endpoint ?? null) : null;
	const engine = d.engines[0]?.key ?? "analytic";
	return usePricing({
		endpoint: values ? endpoint : null,
		body: values ? d.toRequest(values, engine) : null,
		enabled: Boolean(values && endpoint),
	});
}

/**
 * A hand-written catalog product: bump each of its market fields present in
 * its form and reprice through its own endpoint, at its defaults and seed.
 * The error of a change is the prices' errors combined (conservative: the
 * draws are common, so the true paired error is smaller).
 */
export function useNativeRiskProfile(d: ProductDescriptor) {
	const base = useProductPrice(d, d.defaults);
	const up = useProductPrice(d, bumped(d, SPOT_UP));
	const down = useProductPrice(d, bumped(d, SPOT_DOWN));
	const others = [
		useProductPrice(d, bumped(d, BUMPS[0]!)),
		useProductPrice(d, bumped(d, BUMPS[1]!)),
		useProductPrice(d, bumped(d, BUMPS[2]!)),
		useProductPrice(d, bumped(d, BUMPS[3]!)),
		useProductPrice(d, bumped(d, BUMPS[4]!)),
	];
	const all = [base, up, down, ...others];
	const isLoading = all.some((q) => q.isLoading);
	const error = all.find((q) => q.error)?.error ?? null;
	const b0 = base.data;
	const rows: RiskRow[] = [];
	if (b0 && !isLoading) {
		const se = (x?: { mc_std_error: number }) => x?.mc_std_error ?? 0;
		const row = (factor: string, bump: string, change: number, err: number) =>
			rows.push({
				factor,
				bump,
				change,
				std_error: err,
				position: classify(change, err, b0.npv),
			});
		if (up.data && down.data) {
			const [u, dn] = [up.data, down.data];
			row(
				"Spot (delta)",
				"+/-1% of spot",
				(u.npv - dn.npv) / 2,
				Math.hypot(se(u), se(dn)) / 2,
			);
			row(
				"Spot convexity (gamma)",
				"+/-1% of spot",
				u.npv + dn.npv - 2 * b0.npv,
				Math.hypot(se(u), se(dn), 2 * se(b0)),
			);
		}
		BUMPS.forEach((b, i) => {
			const r = others[i]!.data;
			if (r) row(b.factor, b.bump, r.npv - b0.npv, Math.hypot(se(r), se(b0)));
		});
	}
	return {
		isLoading,
		error,
		data: b0
			? {
					rows,
					reference: "the product's default inputs on the pricing page",
					method:
						"Bump and reprice through the product's own pricer, same seed; errors of the prices combined.",
				}
			: undefined,
	};
}
