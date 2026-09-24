import type {
	Instrument,
	Portfolio,
	PortfolioCurrency,
	Trade,
} from "@/shared/api";

/**
 * The ledger, front side. The accounting (weighted average cost, realised
 * P&L) is the server's (api/app/portfolio_ledger.py); the front only needs
 * what it takes to build a trade and to know what is held.
 */

/** The currencies a portfolio can be reported in (ECB reference rates). */
export const CURRENCIES: PortfolioCurrency[] = [
	"EUR",
	"USD",
	"GBP",
	"JPY",
	"CHF",
];

const uid = () =>
	crypto.randomUUID?.() ?? Math.random().toString(36).slice(2, 14);

/** Net quantity per instrument: a sale is a negative quantity. */
export function netQuantities(trades: Trade[]): Map<string, number> {
	const out = new Map<string, number>();
	for (const t of trades) {
		const q = (out.get(t.instrument_id) ?? 0) + t.quantity;
		out.set(t.instrument_id, Math.abs(q) < 1e-9 ? 0 : q);
	}
	return out;
}

/** One instrument per listed ticker, so every trade on it aggregates. */
export const equityInstrumentId = (ticker: string) =>
	`EQ:${ticker.trim().toUpperCase()}`;

export function equityInstrument(ticker: string, name?: string): Instrument {
	const t = ticker.trim().toUpperCase();
	return {
		id: equityInstrumentId(t),
		label: name ? `${t} — ${name}` : t,
		spec: { kind: "equity", ticker: t },
	};
}

export function derivativeInstrument(
	label: string,
	spec: Omit<Extract<Instrument["spec"], { kind: "derivative" }>, "kind">,
): Instrument {
	return { id: uid(), label, spec: { kind: "derivative", ...spec } };
}

export type TradeDraft = Omit<Trade, "id" | "created_at">;

/**
 * The ledger after booking a trade: the instrument is added if new (an
 * equity already held is not duplicated), the trade appended. Nothing is
 * ever edited in place — a correction is another trade, or a deletion.
 */
export function bookTrade(
	pf: Portfolio,
	instrument: Instrument,
	draft: TradeDraft,
): Pick<Portfolio, "instruments" | "transactions" | "base_currency"> {
	const instruments = pf.instruments ?? [];
	const known = instruments.some((i) => i.id === instrument.id);
	return {
		base_currency: pf.base_currency,
		instruments: known ? instruments : [...instruments, instrument],
		transactions: [
			...(pf.transactions ?? []),
			{ ...draft, id: uid(), created_at: new Date().toISOString() },
		],
	};
}

/**
 * The ledger without a trade. An instrument no trade points to any more
 * goes too — except that removing a trade never touches the others.
 */
export function deleteTrade(
	pf: Portfolio,
	tradeId: string,
): Pick<Portfolio, "instruments" | "transactions" | "base_currency"> {
	const transactions = (pf.transactions ?? []).filter((t) => t.id !== tradeId);
	const used = new Set(transactions.map((t) => t.instrument_id));
	return {
		base_currency: pf.base_currency,
		instruments: (pf.instruments ?? []).filter((i) => used.has(i.id)),
		transactions,
	};
}

/**
 * The ledger without a position at all: every trade on the instrument goes,
 * and the instrument with them — for a position booked by mistake. Closing
 * a real position is a sale, not this.
 */
export function deletePosition(
	pf: Portfolio,
	instrumentId: string,
): Pick<Portfolio, "instruments" | "transactions" | "base_currency"> {
	return {
		base_currency: pf.base_currency,
		instruments: (pf.instruments ?? []).filter((i) => i.id !== instrumentId),
		transactions: (pf.transactions ?? []).filter(
			(t) => t.instrument_id !== instrumentId,
		),
	};
}

/** The ticker a position moves with: its own, or its derivative's underlying. */
export function underlyingOf(instrument: Instrument | undefined): string {
	if (!instrument) return "—";
	return instrument.spec.kind === "equity"
		? instrument.spec.ticker
		: (instrument.spec.underlying ?? instrument.label);
}
