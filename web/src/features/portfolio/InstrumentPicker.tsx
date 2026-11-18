import { useEffect, useState } from "react";
import type { Counterparty, Instrument } from "@/shared/api";
import {
	equityInstrument,
	rateFxInstrument,
	scriptInstrument,
} from "@/shared/portfolio";
import { Label } from "@/shared/ui";
import { NettingSetSelect } from "./NettingSetSelect";
import { RateFxFields, type RateFxTerms } from "./RateFxFields";
import {
	type InitialScript,
	ScriptFields,
	type ScriptTerms,
} from "./ScriptFields";
import { TickerPicker } from "./TickerPicker";

type Kind = "equity" | "script" | "rate_fx";

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
 * Defines the instrument of a new trade: a stock (market → ticker), a
 * product of the script library (on a listed underlying, or on a curve), or
 * a rate or FX trade — the last two with the netting set they are traded
 * under, if they face a counterparty.
 */
export function InstrumentPicker({
	tradeDate,
	counterparties,
	initialDerivative,
	onChange,
}: {
	tradeDate: string;
	counterparties: Counterparty[];
	initialDerivative?: InitialScript;
	onChange: (picked: Picked) => void;
}) {
	const [kind, setKind] = useState<Kind>(
		initialDerivative ? "script" : "equity",
	);
	const [ticker, setTicker] = useState<{ t: string; name?: string }>({ t: "" });
	const [script, setScript] = useState<ScriptTerms | null>(null);
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
			kind === "script"
				? script && scriptInstrument(script.label, script.spec)
				: rateFx && rateFxInstrument(rateFx.label, rateFx.spec);
		// A contract's price is what it is worth that day, which can be
		// negative (a swap, a forward).
		onChange({
			instrument: made && { ...made, netting_set_id: nettingSet },
			closeTicker: "",
			value: kind === "rate_fx" ? rateFx?.value : script?.value,
			contract: true,
		});
	}, [kind, ticker, script, rateFx, nettingSet, onChange]);

	return (
		<div className="flex flex-col gap-3">
			<select
				aria-label="Instrument type"
				className="h-9 w-fit rounded-sm border border-hairline bg-surface px-2 text-sm text-ink"
				value={kind}
				onChange={(e) => setKind(e.target.value as Kind)}
			>
				<option value="equity">Stock / index</option>
				<option value="script">Library product (equity or rates)</option>
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
			{kind === "script" && (
				<ScriptFields
					tradeDate={tradeDate}
					initial={initialDerivative}
					onChange={setScript}
				/>
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
