import { useEffect, useMemo, useState } from "react";
import type { RateFxDraft, RateFxSpec } from "@/shared/api";
import { useRateFxQuote } from "@/shared/api";
import { Input, Label } from "@/shared/ui";
import { PRODUCTS, type Product, plusYears } from "./rateFxTerms";

export type RateFxTerms = { label: string; spec: RateFxSpec; value: number };

const box =
	"h-9 rounded-sm border border-hairline bg-surface px-2 text-sm text-ink";

/**
 * The terms of a rate or FX trade. A rate left empty is set at the market of
 * the trade date by the API (par rate, at-the-money strike, forward exchange
 * rate), which also says what the contract is then worth: the default price.
 */
export function RateFxFields({
	tradeDate,
	onChange,
}: {
	tradeDate: string;
	onChange: (terms: RateFxTerms | null) => void;
}) {
	const [product, setProduct] = useState<Product>("swap");
	const [currency, setCurrency] = useState<"USD" | "EUR">("USD");
	const [notional, setNotional] = useState("10000000");
	const [payer, setPayer] = useState(true);
	const [years, setYears] = useState("5");
	const [expiry, setExpiry] = useState("");
	const [delivery, setDelivery] = useState("");
	const [rate, setRate] = useState("");
	const meta = PRODUCTS[product];
	const fx = product === "fx_forward" || product === "cross_currency_swap";
	// An option on a swap: it has an expiry (its first exercise date).
	const optional = product === "swaption" || product === "bermudan";

	const draft = useMemo<RateFxDraft | null>(() => {
		const n = Number(notional);
		const y = Number(years);
		const start = optional ? expiry : tradeDate;
		const maturity =
			product === "fx_forward"
				? delivery
				: start && plusYears(start, Math.round(y));
		if (!(n > 0) || !start || !maturity || maturity <= start) return null;
		if (product !== "fx_forward" && !(y >= 1 && y <= 30)) return null;
		if (optional && expiry <= tradeDate) return null;
		return {
			product,
			currency: fx ? "EUR" : currency,
			notional: n,
			payer,
			start,
			maturity,
			fixed_rate: rate === "" ? null : Number(rate) / (meta.percent ? 100 : 1),
		};
	}, [
		product,
		currency,
		notional,
		payer,
		years,
		expiry,
		delivery,
		rate,
		tradeDate,
		fx,
		optional,
		meta.percent,
	]);
	const quote = useRateFxQuote(draft, tradeDate);

	useEffect(() => {
		onChange(draft && quote.data ? quote.data : null);
	}, [draft, quote.data, onChange]);

	const market = quote.data?.spec.fixed_rate;
	return (
		<div className="flex flex-col gap-3">
			<div className="grid grid-cols-2 gap-3 sm:grid-cols-4">
				<label className="flex flex-col gap-1">
					<Label>Product</Label>
					<select
						className={box}
						value={product}
						onChange={(e) => {
							setProduct(e.target.value as Product);
							setRate("");
						}}
					>
						{Object.entries(PRODUCTS).map(([k, p]) => (
							<option key={k} value={k}>
								{p.label}
							</option>
						))}
					</select>
				</label>
				<label className="flex flex-col gap-1">
					<Label>Side</Label>
					<select
						className={box}
						value={payer ? "0" : "1"}
						onChange={(e) => setPayer(e.target.value === "0")}
					>
						<option value="0">{meta.sides[0]}</option>
						<option value="1">{meta.sides[1]}</option>
					</select>
				</label>
				<label className="flex flex-col gap-1">
					<Label>Notional ({fx ? "EUR" : currency})</Label>
					<Input
						type="number"
						min={0}
						step="any"
						value={notional}
						onChange={(e) => setNotional(e.target.value)}
					/>
				</label>
				{!fx && (
					<label className="flex flex-col gap-1">
						<Label>Currency</Label>
						<select
							className={box}
							value={currency}
							onChange={(e) => setCurrency(e.target.value as "USD" | "EUR")}
						>
							<option>USD</option>
							<option>EUR</option>
						</select>
					</label>
				)}
				{optional && (
					<label className="flex flex-col gap-1">
						<Label>
							{product === "bermudan" ? "First exercise" : "Expiry"}
						</Label>
						<Input
							type="date"
							min={tradeDate}
							value={expiry}
							onChange={(e) => setExpiry(e.target.value)}
						/>
					</label>
				)}
				{product === "fx_forward" ? (
					<label className="flex flex-col gap-1">
						<Label>Delivery</Label>
						<Input
							type="date"
							min={tradeDate}
							value={delivery}
							onChange={(e) => setDelivery(e.target.value)}
						/>
					</label>
				) : (
					<label className="flex flex-col gap-1">
						<Label>{optional ? "Swap length" : "Maturity"} (years)</Label>
						<Input
							type="number"
							min={1}
							max={30}
							step={1}
							value={years}
							onChange={(e) => setYears(e.target.value)}
						/>
					</label>
				)}
				<label className="flex flex-col gap-1">
					<Label>{meta.rate}</Label>
					<Input
						type="number"
						step="any"
						value={rate}
						placeholder={
							market == null
								? "at the market"
								: (market * (meta.percent ? 100 : 1)).toFixed(4)
						}
						onChange={(e) => setRate(e.target.value)}
					/>
				</label>
			</div>
			<p className="text-2xs text-ink-muted">
				{quote.error
					? quote.error.message
					: quote.data
						? `${quote.data.label}. Worth ${quote.data.value.toLocaleString("en-US", { maximumFractionDigits: 0 })} ${quote.data.currency} on the trade date: the default price.`
						: "A rate left empty is set at the market of the trade date. Both legs of a swap pay once a year."}{" "}
				One position is the whole contract: buy 1 to enter it as described, sell
				1 to take the other side.
			</p>
		</div>
	);
}
