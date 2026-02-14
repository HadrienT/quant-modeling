import { useMemo } from "react";
import type { Dispatch, SetStateAction } from "react";
import PnlChart, { PnlSeries } from "../components/PnlChart";
import PortfolioBuilder, { OptionLeg, PortfolioDraft } from "../components/PortfolioBuilder";

const palette = ["#1f8a70", "#ff8c42", "#2f4858", "#f6c453", "#8c5e58"];

type VisualizeProps = {
	singleLeg: OptionLeg;
	setSingleLeg: Dispatch<SetStateAction<OptionLeg>>;
	portfolio: OptionLeg[];
	setPortfolio: Dispatch<SetStateAction<OptionLeg[]>>;
	draft: PortfolioDraft;
	setDraft: Dispatch<SetStateAction<PortfolioDraft>>;
	focusAggregate: boolean;
	setFocusAggregate: Dispatch<SetStateAction<boolean>>;
	spotMin: number;
	setSpotMin: Dispatch<SetStateAction<number>>;
	spotMax: number;
	setSpotMax: Dispatch<SetStateAction<number>>;
};

const buildSpotGrid = (min: number, max: number, steps: number) => {
	const out: number[] = [];
	const dx = (max - min) / steps;
	for (let i = 0; i <= steps; i += 1) {
		out.push(min + dx * i);
	}
	return out;
};

const payoffAt = (spot: number, leg: OptionLeg) => {
	const sign = leg.side === "long" ? 1 : -1;
	const qty = leg.quantity;
	if (leg.type === "stock") {
		return sign * qty * (spot - leg.strike);
	}
	const intrinsic = leg.type === "call" ? Math.max(spot - leg.strike, 0) : Math.max(leg.strike - spot, 0);
	return sign * qty * (intrinsic - leg.premium);
};

const aggregatePayoff = (spot: number, legs: OptionLeg[]) =>
	legs.reduce((sum, leg) => sum + payoffAt(spot, leg), 0);

export default function Visualize({
	singleLeg,
	setSingleLeg,
	portfolio,
	setPortfolio,
	draft,
	setDraft,
	focusAggregate,
	setFocusAggregate,
	spotMin,
	setSpotMin,
	spotMax,
	setSpotMax
}: VisualizeProps) {
	const grid = useMemo(() => buildSpotGrid(spotMin, spotMax, 80), [spotMin, spotMax]);

	const singleSeries: PnlSeries = useMemo(
		() => ({
			id: "single",
			label: singleLeg.name,
			color: palette[0],
			points: grid.map((spot) => ({ x: spot, y: payoffAt(spot, singleLeg) }))
		}),
		[grid, singleLeg]
	);

	const portfolioSeries: PnlSeries = useMemo(
		() => ({
			id: "portfolio",
			label: "Portfolio",
			color: palette[1],
			points: grid.map((spot) => ({ x: spot, y: aggregatePayoff(spot, portfolio) }))
		}),
		[grid, portfolio]
	);

	const legSeries = useMemo(
		() =>
			portfolio.map((leg, idx) => ({
				id: leg.id,
				label: leg.name,
				color: palette[(idx + 2) % palette.length],
				points: grid.map((spot) => ({ x: spot, y: payoffAt(spot, leg) }))
			})),
		[grid, portfolio]
	);

	const portfolioChartSeries = useMemo<PnlSeries[]>(() => {
		if (!focusAggregate) {
			return [portfolioSeries, ...legSeries];
		}
		return [
			{ ...portfolioSeries, opacity: 1 },
			...legSeries.map((s) => ({ ...s, opacity: 0.15 }))
		];
	}, [focusAggregate, portfolioSeries, legSeries]);

	const addLeg = () => {
		setPortfolio((prev) => [
			...prev,
			{
				id: `leg-${prev.length + 1}`,
				name: draft.name.trim() || `Leg ${prev.length + 1}`,
				type: draft.type,
				side: draft.side,
				strike: draft.strike,
				premium: draft.premium,
				quantity: draft.quantity
			}
		]);
	};

	const clearPortfolio = () => {
		setPortfolio([]);
	};

	const removeLeg = (id: string) => {
		setPortfolio((prev) => prev.filter((leg) => leg.id !== id));
	};

	return (
		<>
			<section className="hero">
				<div>
					<h1>Option payoffs, visualized as a living portfolio.</h1>
					<p>
						Explore single option shapes, then layer legs into portfolios. Every tweak updates the curves instantly so you
						can see convexity, hedges, and cost at a glance.
					</p>
				</div>
			</section>

			<section className="card">
				<h2>Spot range</h2>
				<div className="grid-2">
					<label className="field">
						Min spot
						<input
							type="number"
							value={spotMin}
							onChange={(event) => setSpotMin(Number(event.target.value))}
						/>
					</label>
					<label className="field">
						Max spot
						<input
							type="number"
							value={spotMax}
							onChange={(event) => setSpotMax(Number(event.target.value))}
						/>
					</label>
				</div>
			</section>

			<section className="section-grid">
				<div className="card">
					<h2>Single option payoff</h2>
					<div className="grid-2">
						<label className="field">
							Name
							<input
								value={singleLeg.name}
								onChange={(event) => setSingleLeg((prev) => ({ ...prev, name: event.target.value }))}
							/>
						</label>
						<label className="field">
							Type
							<select
								value={singleLeg.type}
								onChange={(event) =>
									setSingleLeg((prev) => ({ ...prev, type: event.target.value as OptionLeg["type"] }))
								}
							>
								<option value="call">Call</option>
								<option value="put">Put</option>
								<option value="stock">Stock</option>
							</select>
						</label>
						<label className="field">
							Side
							<select
								value={singleLeg.side}
								onChange={(event) =>
									setSingleLeg((prev) => ({ ...prev, side: event.target.value as OptionLeg["side"] }))
								}
							>
								<option value="long">Long</option>
								<option value="short">Short</option>
							</select>
						</label>
						<label className="field">
							Strike
							<input
								type="number"
								value={singleLeg.strike}
								onChange={(event) => setSingleLeg((prev) => ({ ...prev, strike: Number(event.target.value) }))}
							/>
						</label>
						<label className="field">
							Premium
							<input
								type="number"
								value={singleLeg.premium}
								onChange={(event) => setSingleLeg((prev) => ({ ...prev, premium: Number(event.target.value) }))}
							/>
						</label>
						<label className="field">
							Quantity
							<input
								type="number"
								value={singleLeg.quantity}
								onChange={(event) => setSingleLeg((prev) => ({ ...prev, quantity: Number(event.target.value) }))}
							/>
						</label>
					</div>
					<div className="chart-wrap">
						<PnlChart title="Single option" series={[singleSeries]} />
					</div>
				</div>

				<div className="card">
					<h2>Portfolio payoff</h2>
					<PortfolioBuilder
						draft={draft}
						onDraftChange={setDraft}
						onAdd={addLeg}
						onClear={clearPortfolio}
						legs={portfolio}
						onRemove={removeLeg}
					/>
					<div style={{ display: "flex", alignItems: "center", gap: 10, marginTop: 12 }}>
						<label className="field" style={{ display: "flex", alignItems: "center", gap: 8 }}>
							<input
								type="checkbox"
								checked={focusAggregate}
								onChange={(event) => setFocusAggregate(event.target.checked)}
							/>
							Focus aggregate PnL
						</label>
					</div>
					<div className="chart-wrap">
						<PnlChart title="Portfolio" series={portfolioChartSeries} showLegend />
					</div>
				</div>
			</section>
		</>
	);
}
