import { useEffect, useState } from "react";
import type { DerivativeSpec, PortfolioCurrency } from "@/shared/api";
import { useTickerClose } from "@/shared/api";
import { CATALOG_BY_KEY, type EngineKey, ParamForm } from "@/shared/products";
import { CURRENCIES } from "@/shared/portfolio";
import { Input, Label } from "@/shared/ui";
import { TickerPicker } from "./TickerPicker";

/**
 * Catalog key → the pricing registry's product id (api/app/valuation.py).
 * Single-underlying products only: each day's mark takes the underlying's
 * close as spot, so a product needs exactly one listed underlying.
 */
const PRODUCTS: Record<string, string> = {
	vanilla: "vanilla",
	american: "american_vanilla",
	quanto: "quanto",
	asian: "asian",
	barrier: "barrier",
	digital: "digital",
	lookback: "lookback",
	future: "future",
};

export type DerivativeDraft = {
	label: string;
	spec: Omit<DerivativeSpec, "kind">;
};

const YEAR = 365.25 * 86_400_000;
/** Set from the trade (spot, maturity) or by the valuation (engine). */
const DERIVED = ["spot", "maturity", "engine", "n_paths", "seed", "tree_steps"];

/**
 * A derivative's contract terms. Its market inputs (spot, time to expiry,
 * rate, dividend, vol) are only the trade-date values: every valuation
 * replaces them with the day's market (the methodology says how).
 */
export function DerivativeFields({
	tradeDate,
	onChange,
}: {
	tradeDate: string;
	onChange: (draft: DerivativeDraft | null) => void;
}) {
	const [key, setKey] = useState("vanilla");
	const descriptor = CATALOG_BY_KEY.get(key)!;
	const [values, setValues] = useState<Record<string, unknown>>(
		descriptor.defaults,
	);
	const [underlying, setUnderlying] = useState("");
	const [expiry, setExpiry] = useState("");
	const [currency, setCurrency] = useState<PortfolioCurrency | null>(null);
	const close = useTickerClose(underlying, tradeDate);
	const ccy = currency ?? (close.data?.currency as PortfolioCurrency) ?? "USD";

	useEffect(() => {
		const years = (Date.parse(expiry) - Date.parse(tradeDate)) / YEAR;
		if (!underlying || !(years > 0)) return onChange(null);
		// The product's default engine unless it is Monte-Carlo: the mark is
		// recomputed for every day of the history, so a closed form or a tree.
		const chosen = values.engine as EngineKey | undefined;
		const engine =
			chosen && chosen !== "mc"
				? chosen
				: (descriptor.engines.find((e) => e.key !== "mc")?.key ?? "mc");
		const params = descriptor.toRequest(values, engine) as Record<
			string,
			unknown
		>;
		params.maturity = years;
		if (close.data) params.spot = close.data.close;
		const strike = params.strike != null ? ` K=${params.strike}` : "";
		onChange({
			label: `${underlying} ${descriptor.label}${strike} ${expiry}`,
			spec: {
				product: PRODUCTS[key]!,
				params,
				underlying,
				expiry,
				currency: ccy,
			},
		});
	}, [
		key,
		values,
		underlying,
		expiry,
		ccy,
		tradeDate,
		close.data,
		descriptor,
		onChange,
	]);

	return (
		<div className="flex flex-col gap-3">
			<div className="grid grid-cols-2 gap-3 sm:grid-cols-4">
				<label className="flex flex-col gap-1">
					<Label>Product</Label>
					<select
						className="h-9 rounded-sm border border-hairline bg-surface px-2 text-sm text-ink"
						value={key}
						onChange={(e) => {
							setKey(e.target.value);
							setValues(CATALOG_BY_KEY.get(e.target.value)!.defaults);
						}}
					>
						{Object.keys(PRODUCTS).map((k) => (
							<option key={k} value={k}>
								{CATALOG_BY_KEY.get(k)?.label ?? k}
							</option>
						))}
					</select>
				</label>
				<TickerPicker
					label="Underlying"
					ticker={underlying}
					onTicker={(t) => setUnderlying(t)}
				/>
				<label className="flex flex-col gap-1">
					<Label>Expiry</Label>
					<Input
						type="date"
						value={expiry}
						min={tradeDate}
						onChange={(e) => setExpiry(e.target.value)}
					/>
				</label>
				<label className="flex flex-col gap-1">
					<Label>Currency</Label>
					<select
						className="h-9 rounded-sm border border-hairline bg-surface px-2 text-sm text-ink"
						value={ccy}
						onChange={(e) => setCurrency(e.target.value as PortfolioCurrency)}
					>
						{CURRENCIES.map((c) => (
							<option key={c}>{c}</option>
						))}
					</select>
				</label>
			</div>
			<p className="text-2xs text-ink-muted">
				Spot comes from the underlying&apos;s close and maturity from the
				expiry. Rate, dividend and volatility below are fallbacks: every
				valuation uses that day&apos;s market where the database has it, and
				each position says which inputs it used.
			</p>
			<ParamForm
				key={key}
				descriptor={descriptor}
				values={values}
				onChange={setValues}
				hide={DERIVED}
			/>
		</div>
	);
}
