export type OptionSide = "long" | "short";
export type OptionType = "call" | "put" | "stock";

export type OptionLeg = {
	id: string;
	name: string;
	type: OptionType;
	side: OptionSide;
	strike: number;
	premium: number;
	quantity: number;
};

export type PortfolioDraft = Omit<OptionLeg, "id">;

type PortfolioBuilderProps = {
	draft: PortfolioDraft;
	onDraftChange: (draft: PortfolioDraft) => void;
	onAdd: () => void;
	onClear: () => void;
	legs: OptionLeg[];
	onRemove: (id: string) => void;
};

export default function PortfolioBuilder({
	draft,
	onDraftChange,
	onAdd,
	onClear,
	legs,
	onRemove
}: PortfolioBuilderProps) {
	return (
		<div>
			<div className="grid-2">
				<label className="field">
					Name
					<input
						value={draft.name}
						onChange={(event) => onDraftChange({ ...draft, name: event.target.value })}
					/>
				</label>
				<label className="field">
					Type
					<select
						value={draft.type}
						onChange={(event) => onDraftChange({ ...draft, type: event.target.value as OptionType })}
					>
						<option value="call">Call</option>
						<option value="put">Put</option>
						<option value="stock">Stock</option>
					</select>
				</label>
				<label className="field">
					Side
					<select
						value={draft.side}
						onChange={(event) => onDraftChange({ ...draft, side: event.target.value as OptionSide })}
					>
						<option value="long">Long</option>
						<option value="short">Short</option>
					</select>
				</label>
				<label className="field">
					Strike
					<input
						type="number"
						value={draft.strike}
						onChange={(event) => onDraftChange({ ...draft, strike: Number(event.target.value) })}
					/>
				</label>
				<label className="field">
					Premium
					<input
						type="number"
						value={draft.premium}
						onChange={(event) => onDraftChange({ ...draft, premium: Number(event.target.value) })}
					/>
				</label>
				<label className="field">
					Quantity
					<input
						type="number"
						value={draft.quantity}
						onChange={(event) => onDraftChange({ ...draft, quantity: Number(event.target.value) })}
					/>
				</label>
			</div>
			<div style={{ display: "flex", gap: 12, marginTop: 14 }}>
				<button className="button" type="button" onClick={onAdd}>
					Add leg
				</button>
				<button
					className="button secondary"
					type="button"
					onClick={onClear}
				>
					Clear portfolio
				</button>
			</div>
			<div className="portfolio-list">
				{legs.map((leg) => (
					<div key={leg.id} className="portfolio-item">
						<div>
							<p>
								{leg.name} · {leg.side} {leg.type}
							</p>
							<span>
								K {leg.strike} · premium {leg.premium} · qty {leg.quantity}
							</span>
						</div>
						<button className="button ghost" type="button" onClick={() => onRemove(leg.id)}>
							Remove
						</button>
					</div>
				))}
			</div>
		</div>
	);
}
