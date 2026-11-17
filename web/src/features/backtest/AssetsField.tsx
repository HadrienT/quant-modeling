import { X } from "lucide-react";
import type { BacktestUniverse } from "@/shared/api";
import { Badge, Combobox, Label, Segmented } from "@/shared/ui";
import type { BacktestView } from "./state";

/**
 * What the strategy may hold: assets chosen by hand, or every S&P 500 member
 * at each date (prices.sp500_membership), each held only while it is one. The
 * second removes the look-ahead of today's composition, not the survivorship:
 * a company since delisted has no prices. Both biases are said where they
 * apply.
 */
export function AssetsField({
	universe,
	draft,
	set,
}: {
	universe: BacktestUniverse;
	draft: BacktestView;
	set: (patch: Partial<BacktestView>) => void;
}) {
	const funds = new Set(universe.etfs.map((a) => a.ticker));
	const stocks = draft.tickers.filter((t) => !funds.has(t));
	const offered = [...universe.etfs, ...universe.stocks]
		.filter((a) => !draft.tickers.includes(a.ticker))
		.map((a) => ({ value: a.ticker, label: `${a.ticker} · ${a.name}` }));

	return (
		<div>
			<div className="flex flex-wrap items-center gap-3">
				<Label>Assets</Label>
				<Segmented
					label="Universe"
					options={[
						{ value: "chosen", label: "Chosen" },
						{ value: "sp500", label: "S&P 500 members at each date" },
					]}
					value={draft.universe}
					onChange={(u) => set({ universe: u })}
				/>
			</div>
			{draft.universe === "sp500" ? (
				<p className="mt-1.5 text-2xs text-warning">
					Every S&P 500 member of the period that has prices, each held only
					while it is a member: the composition is the one known on each date,
					with no look-ahead. Survivorship remains: a company since delisted has
					no prices and is missing, which biases the result upwards. Risk parity
					and the overfitting grid are not offered here: on some 500 names they
					take minutes.
				</p>
			) : (
				<>
					<div className="mt-1 flex flex-wrap items-center gap-1.5">
						{draft.tickers.map((t) => (
							<Badge key={t} tone={funds.has(t) ? "accent" : "warning"}>
								{t}
								<button
									type="button"
									aria-label={`Remove ${t}`}
									onClick={() =>
										set({ tickers: draft.tickers.filter((x) => x !== t) })
									}
								>
									<X className="size-3" />
								</button>
							</Badge>
						))}
						<div className="w-56">
							<Combobox
								options={offered}
								value={null}
								onChange={(t) => set({ tickers: [...draft.tickers, t] })}
								placeholder="Add a fund or a stock…"
							/>
						</div>
					</div>
					{stocks.length > 0 && (
						<p className="mt-1.5 text-2xs text-warning">
							{stocks.join(", ")}: single stocks are picked with hindsight, and
							the database only has the companies that are still listed. A
							basket of funds does not carry that bias.
						</p>
					)}
				</>
			)}
		</div>
	);
}
