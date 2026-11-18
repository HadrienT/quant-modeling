import { useEffect, useMemo, useState } from "react";
import type { ScriptQuoteRequest, ScriptSpec } from "@/shared/api";
import { useScriptQuote } from "@/shared/api";
import {
	BOOKABLE_SCRIPTS as BOOKABLE,
	type ScriptedProduct,
} from "@/shared/products";
import { Input, Label } from "@/shared/ui";
import { ErrorState } from "@/shared/ui/states";
import { TickerPicker } from "./TickerPicker";

export type ScriptTerms = { label: string; spec: ScriptSpec; value: number };

/** A product carried in from the Price page (`/portfolio?trade=`). */
export type InitialScript = {
	product: string;
	values: Record<string, unknown>;
};

const GOVERNMENT = ["USD", "EUR", "JPY"] as const;
const box =
	"h-9 rounded-sm border border-hairline bg-surface px-2 text-sm text-ink";
const DEBOUNCE_MS = 400;

const slugOf = (key: string) => key.replace(/^script-/, "");

function defaults(p: ScriptedProduct): Record<string, string> {
	return Object.fromEntries(
		p.params.map((prm) => [
			prm.name,
			String(
				prm.unit === "percent" ? +(prm.default * 100).toFixed(6) : prm.default,
			),
		]),
	);
}

/**
 * A product of the script library as a position (issue 313, lot b3): its
 * terms, and the ticker its spot() reads -- or, for a rates product on a
 * government curve, the currency. The API renders its script on the trade
 * date (its dates are then the contract's) and values it that day: the
 * default price.
 */
export function ScriptFields({
	tradeDate,
	initial,
	onChange,
}: {
	tradeDate: string;
	initial?: InitialScript;
	onChange: (terms: ScriptTerms | null) => void;
}) {
	const start = BOOKABLE.find((p) => p.slug === slugOf(initial?.product ?? ""));
	const [slug, setSlug] = useState(start?.slug ?? "european-call");
	const product = BOOKABLE.find((p) => p.slug === slug)!;
	const [terms, setTerms] = useState<Record<string, string>>(() => {
		const base = defaults(product);
		for (const k of Object.keys(base))
			if (start && initial?.values[k] != null)
				base[k] = String(initial.values[k]);
		return base;
	});
	const [ticker, setTicker] = useState(
		String(initial?.values.tickers ?? "").split(/[\s,;]+/)[0] ?? "",
	);
	const [currency, setCurrency] = useState<string>(
		String(initial?.values.currency ?? "USD"),
	);
	const rates = product.market === "rates";
	const government = rates && product.curve !== "sofr";

	const request = useMemo<ScriptQuoteRequest | null>(() => {
		const out: Record<string, number> = {};
		for (const prm of product.params) {
			const v = Number(terms[prm.name]);
			if (terms[prm.name] === "" || !Number.isFinite(v)) return null;
			out[prm.name] = prm.unit === "percent" ? v / 100 : v;
		}
		if (!rates && !ticker) return null;
		return {
			product: slug,
			terms: out,
			trade_date: tradeDate,
			underlying: rates ? null : ticker,
			currency: government ? currency : null,
		};
	}, [product, terms, ticker, currency, slug, tradeDate, rates, government]);

	// Typing a term asks for a quote once the typing stops.
	const [asked, setAsked] = useState(request);
	useEffect(() => {
		const id = setTimeout(() => setAsked(request), DEBOUNCE_MS);
		return () => clearTimeout(id);
	}, [request]);
	const quote = useScriptQuote(asked);

	useEffect(() => {
		onChange(request && asked === request && quote.data ? quote.data : null);
	}, [request, asked, quote.data, onChange]);

	return (
		<div className="flex flex-col gap-3">
			<div className="grid grid-cols-2 gap-3 sm:grid-cols-4">
				<label className="col-span-2 flex flex-col gap-1">
					<Label>Product</Label>
					<select
						className={box}
						value={slug}
						onChange={(e) => {
							const next = BOOKABLE.find((p) => p.slug === e.target.value)!;
							setSlug(next.slug);
							setTerms(defaults(next));
						}}
					>
						{BOOKABLE.map((p) => (
							<option key={p.slug} value={p.slug}>
								{p.title}
							</option>
						))}
					</select>
				</label>
				{!rates && (
					<TickerPicker
						label="Underlying"
						ticker={ticker}
						onTicker={(t) => setTicker(t)}
					/>
				)}
				{government && (
					<label className="flex flex-col gap-1">
						<Label>Government curve</Label>
						<select
							className={box}
							value={currency}
							onChange={(e) => setCurrency(e.target.value)}
						>
							{GOVERNMENT.map((c) => (
								<option key={c}>{c}</option>
							))}
						</select>
					</label>
				)}
				{product.params.map((prm) => (
					<label key={prm.name} className="flex flex-col gap-1">
						<Label>
							{prm.label}
							{prm.unit === "percent" ? " (%)" : ""}
						</Label>
						<Input
							type="number"
							step="any"
							value={terms[prm.name] ?? ""}
							onChange={(e) =>
								setTerms((t) => ({ ...t, [prm.name]: e.target.value }))
							}
						/>
					</label>
				))}
			</div>
			<p className="text-2xs text-ink-muted">
				{product.summary} The script is dated from the trade date: its first
				date falls within the week that follows, and every later mark replays
				its dates.
				{product.curve === "sofr" &&
					" Priced on the USD SOFR curve of the traded swaps, Hull-White calibrated to the traded swaptions."}
			</p>
			{quote.error ? (
				<ErrorState error={quote.error} onRetry={() => quote.refetch()} />
			) : (
				quote.data && (
					<p className="text-2xs text-ink-muted">
						{quote.data.label}: worth {quote.data.value.toFixed(4)}{" "}
						{quote.data.currency} on {tradeDate}, the default price.
					</p>
				)
			)}
		</div>
	);
}
