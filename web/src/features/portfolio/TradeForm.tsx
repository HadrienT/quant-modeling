import { useState } from "react";
import type { Instrument } from "@/shared/api";
import { useTickerClose } from "@/shared/api";
import {
	type TradeDraft,
	derivativeInstrument,
	equityInstrument,
} from "@/shared/portfolio";
import { Button, Input, Label } from "@/shared/ui";
import { type DerivativeDraft, DerivativeFields } from "./DerivativeFields";
import { TickerPicker } from "./TickerPicker";

export type TradePreset = {
	instrument: Instrument;
	side: "buy" | "sell";
	quantity: number;
};

const today = () => new Date().toISOString().slice(0, 10);
const box =
	"h-9 rounded-sm border border-hairline bg-surface px-2 text-sm text-ink";

/**
 * Book a trade. A buy adds to a position, a sale reduces it — and selling
 * more than is held opens a short. From a position's row the instrument is
 * preset; otherwise a stock (market → ticker) or a derivative (catalog
 * product on a listed underlying) is defined here.
 */
export function TradeForm({
	preset,
	onBook,
	onCancel,
}: {
	preset?: TradePreset;
	onBook: (instrument: Instrument, draft: TradeDraft) => void;
	onCancel: () => void;
}) {
	const [kind, setKind] = useState<"equity" | "derivative">("equity");
	const [side, setSide] = useState(preset?.side ?? "buy");
	const [quantity, setQuantity] = useState(String(preset?.quantity ?? ""));
	const [tradeDate, setTradeDate] = useState(today());
	const [price, setPrice] = useState("");
	const [fees, setFees] = useState("0");
	const [note, setNote] = useState("");
	const [ticker, setTicker] = useState<{ t: string; name?: string }>({ t: "" });
	const [derivative, setDerivative] = useState<DerivativeDraft | null>(null);

	const presetTicker =
		preset?.instrument.spec.kind === "equity"
			? preset.instrument.spec.ticker
			: "";
	const closeTicker = preset ? presetTicker : kind === "equity" ? ticker.t : "";
	const close = useTickerClose(closeTicker, tradeDate);
	const unitPrice = price !== "" ? Number(price) : close.data?.close;

	const instrument: Instrument | null = preset
		? preset.instrument
		: kind === "equity"
			? ticker.t
				? equityInstrument(ticker.t, ticker.name)
				: null
			: derivative && derivativeInstrument(derivative.label, derivative.spec);
	const qty = Number(quantity);
	const valid =
		!!instrument &&
		qty > 0 &&
		unitPrice != null &&
		Number.isFinite(unitPrice) &&
		unitPrice >= 0 &&
		Number(fees) >= 0 &&
		tradeDate <= today();

	function submit() {
		if (!valid) return;
		onBook(instrument!, {
			instrument_id: instrument!.id,
			trade_date: tradeDate,
			quantity: side === "buy" ? qty : -qty,
			price: unitPrice!,
			fees: Number(fees),
			note,
		});
	}

	return (
		// A div, not a form: the derivative's terms are ParamForm's own <form>.
		<div className="flex flex-col gap-3 rounded-md border border-hairline bg-surface p-4">
			<div className="flex flex-wrap items-center gap-2">
				<div role="group" aria-label="Side" className="flex gap-1">
					{(["buy", "sell"] as const).map((s) => (
						<Button
							key={s}
							type="button"
							size="sm"
							variant={side === s ? "primary" : "secondary"}
							aria-pressed={side === s}
							onClick={() => setSide(s)}
						>
							{s === "buy" ? "Buy" : "Sell"}
						</Button>
					))}
				</div>
				{preset ? (
					<span className="text-sm text-ink">{preset.instrument.label}</span>
				) : (
					<select
						aria-label="Instrument type"
						className={box}
						value={kind}
						onChange={(e) => setKind(e.target.value as typeof kind)}
					>
						<option value="equity">Stock / index</option>
						<option value="derivative">Derivative</option>
					</select>
				)}
			</div>

			{!preset && kind === "equity" && (
				<div className="grid grid-cols-2 gap-3 sm:grid-cols-4">
					<TickerPicker
						label="Ticker"
						ticker={ticker.t}
						onTicker={(t, name) => setTicker({ t, name })}
					/>
				</div>
			)}
			{!preset && kind === "derivative" && (
				<DerivativeFields tradeDate={tradeDate} onChange={setDerivative} />
			)}

			<div className="grid grid-cols-2 gap-3 sm:grid-cols-5">
				<label className="flex flex-col gap-1">
					<Label>Trade date</Label>
					<Input
						type="date"
						value={tradeDate}
						max={today()}
						onChange={(e) => setTradeDate(e.target.value)}
					/>
				</label>
				<label className="flex flex-col gap-1">
					<Label>Quantity</Label>
					<Input
						type="number"
						min={0}
						step="any"
						value={quantity}
						onChange={(e) => setQuantity(e.target.value)}
					/>
				</label>
				<label className="flex flex-col gap-1">
					<Label>Unit price</Label>
					<Input
						type="number"
						min={0}
						step="any"
						value={price}
						placeholder={close.data ? String(close.data.close) : ""}
						onChange={(e) => setPrice(e.target.value)}
					/>
					{close.data && price === "" && (
						<span className="text-2xs text-ink-muted">
							close of {close.data.date}, {close.data.currency}
						</span>
					)}
				</label>
				<label className="flex flex-col gap-1">
					<Label>Fees</Label>
					<Input
						type="number"
						min={0}
						step="any"
						value={fees}
						onChange={(e) => setFees(e.target.value)}
					/>
				</label>
				<label className="flex flex-col gap-1">
					<Label>Note</Label>
					<Input value={note} onChange={(e) => setNote(e.target.value)} />
				</label>
			</div>

			<div className="flex gap-2">
				<Button type="button" size="sm" disabled={!valid} onClick={submit}>
					Book {side === "buy" ? "purchase" : "sale"}
				</Button>
				<Button type="button" size="sm" variant="ghost" onClick={onCancel}>
					Cancel
				</Button>
			</div>
		</div>
	);
}
