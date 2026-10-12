import { useEffect, useState } from "react";
import type { Counterparty, Instrument } from "@/shared/api";
import {
	derivativeInstrument,
	equityInstrument,
	rateFxInstrument,
} from "@/shared/portfolio";
import { Label } from "@/shared/ui";
import { type DerivativeDraft, DerivativeFields } from "./DerivativeFields";
import { NettingSetSelect } from "./NettingSetSelect";
import { RateFxFields, type RateFxTerms } from "./RateFxFields";
import { TickerPicker } from "./TickerPicker";

type Kind = "equity" | "derivative" | "rate_fx";

/** The instrument of a new trade, and what its form needs to know of it. */
export type Picked = {
	instrument: Instrument | null;
	/** the ticker whose close is the default price (a listed instrument) */
	closeTicker: string;
	/** the default price of an over-the-counter contract: its value */
	value?: number;
	/** one position is one contract, which can be worth less than zero */
	contract: boolean;
};

/**
 * Defines the instrument of a new trade: a stock (market → ticker), an
 * equity derivative (catalog product on a listed underlying), or a rate or
 * FX trade — the last two with the netting set they are traded under, if
 * they face a counterparty.
 */
export function InstrumentPicker({
	tradeDate,
	counterparties,
	onChange,
}: {
	tradeDate: string;
	counterparties: Counterparty[];
	onChange: (picked: Picked) => void;
}) {
	const [kind, setKind] = useState<Kind>("equity");
	const [ticker, setTicker] = useState<{ t: string; name?: string }>({ t: "" });
	const [derivative, setDerivative] = useState<DerivativeDraft | null>(null);
	const [rateFx, setRateFx] = useState<RateFxTerms | null>(null);
	const [nettingSet, setNettingSet] = useState<string | null>(null);

	useEffect(() => {
		if (kind === "equity")
			return onChange({
				instrument: ticker.t ? equityInstrument(ticker.t, ticker.name) : null,
				closeTicker: ticker.t,
				contract: false,
			});
		const made =
			kind === "derivative"
				? derivative && derivativeInstrument(derivative.label, derivative.spec)
				: rateFx && rateFxInstrument(rateFx.label, rateFx.spec);
		onChange({
			instrument: made && { ...made, netting_set_id: nettingSet },
			closeTicker: "",
			value: kind === "rate_fx" ? rateFx?.value : undefined,
			contract: kind === "rate_fx",
		});
	}, [kind, ticker, derivative, rateFx, nettingSet, onChange]);

	return (
		<div className="flex flex-col gap-3">
			<select
				aria-label="Instrument type"
				className="h-9 w-fit rounded-sm border border-hairline bg-surface px-2 text-sm text-ink"
				value={kind}
				onChange={(e) => setKind(e.target.value as Kind)}
			>
				<option value="equity">Stock / index</option>
				<option value="derivative">Equity derivative</option>
				<option value="rate_fx">Rate / FX trade (over the counter)</option>
			</select>
			{kind === "equity" && (
				<div className="grid grid-cols-2 gap-3 sm:grid-cols-4">
					<TickerPicker
						label="Ticker"
						ticker={ticker.t}
						onTicker={(t, name) => setTicker({ t, name })}
					/>
				</div>
			)}
			{kind === "derivative" && (
				<DerivativeFields tradeDate={tradeDate} onChange={setDerivative} />
			)}
			{kind === "rate_fx" && (
				<RateFxFields tradeDate={tradeDate} onChange={setRateFx} />
			)}
			{kind !== "equity" && (
				<label className="flex flex-col gap-1">
					<Label>Counterparty and netting set</Label>
					<NettingSetSelect
						label="Counterparty and netting set"
						counterparties={counterparties}
						value={nettingSet}
						onChange={setNettingSet}
					/>
					<span className="text-2xs text-ink-muted">
						Counterparties are created in the Counterparties tab.
					</span>
				</label>
			)}
		</div>
	);
}
