import type { RatesQuoteSetId, SwapInput, SwaptionInput } from "@/shared/api";
import { Badge, Segmented, Skeleton } from "@/shared/ui";
import { QuotesEditor } from "./QuotesEditor";
import { SwapFields } from "./SwapFields";
import { SwaptionFields } from "./SwaptionFields";
import { type RatesValues, useRatesInputs } from "./useRatesInputs";

const SETS: { value: RatesQuoteSetId; label: string; title: string }[] = [
	{
		value: "usd-sofr",
		label: "USD SOFR · traded",
		title:
			"The swap curve and swaption vols of the swaps and swaptions actually traded (DTCC). One curve: SOFR both discounts and projects.",
	},
	{
		value: "eur-illustrative",
		label: "EUR · illustrative, two curves",
		title:
			"Illustrative €STR and EURIBOR 6M quotes, not market data: the set where the projection curve differs from the discount curve.",
	},
];

/**
 * The form of a rates product: which quotes it is priced on, the contract,
 * and the quotes themselves. Changing the set drops what was edited — a
 * contract and quotes belong to their currency.
 */
export function RatesForm({
	kind,
	values,
	onChange,
}: {
	kind: "swap" | "swaption";
	values: RatesValues;
	onChange: (next: RatesValues) => void;
}) {
	const { setId, set, quotes, contract, edited } = useRatesInputs(kind, values);
	return (
		<div className="flex flex-col gap-4">
			<fieldset className="flex flex-col gap-2">
				<legend className="text-2xs font-semibold tracking-wide text-ink-muted uppercase">
					Market
				</legend>
				<Segmented
					label="Quote set"
					options={SETS}
					value={setId}
					onChange={(id) => onChange({ quote_set: id })}
				/>
				{set.data && (
					<p className="flex flex-wrap items-center gap-2 text-2xs text-ink-secondary">
						{set.data.source === "market" ? (
							<Badge tone="good">Traded prices</Badge>
						) : (
							<Badge tone="warning">Manual input</Badge>
						)}
						{set.data.label}
					</p>
				)}
			</fieldset>
			{set.error ? null : !quotes || !contract ? (
				<Skeleton className="h-40" />
			) : (
				<>
					{kind === "swap" ? (
						<SwapFields
							value={contract as SwapInput}
							onChange={(c) => onChange({ ...values, contract: c })}
						/>
					) : (
						<SwaptionFields
							value={contract as SwaptionInput}
							onChange={(c) => onChange({ ...values, contract: c })}
						/>
					)}
					<QuotesEditor
						key={setId}
						value={quotes}
						withVols={kind === "swaption"}
						edited={edited}
						onApply={(q) => onChange({ ...values, quotes: q })}
						onReset={() => onChange({ ...values, quotes: undefined })}
					/>
				</>
			)}
		</div>
	);
}
