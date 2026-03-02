import { useCallback, useMemo, useState } from "react";
import PnlChart, { PnlSeries } from "../components/PnlChart";
import type { Leg, LegType, StrategyKey } from "./visualize/types";
import { buildGrid, legPayoff, portfolioPayoff, computeMetrics } from "./visualize/payoff";
import { STRATEGIES, STRATEGY_KEYS } from "./visualize/strategies";
import LegCard from "./visualize/LegCard";
import MetricsPanel from "./visualize/MetricsPanel";

const palette = [
	"#1f8a70",
	"#ff8c42",
	"#2f4858",
	"#f6c453",
	"#8c5e58",
	"#6b5b95",
	"#88b04b",
	"#d65076",
];

let _nextId = 1;
const makeId = () => `leg-${_nextId++}`;

const defaultLegs: Leg[] = [
	{
		id: makeId(),
		label: "Call",
		type: "call",
		side: "long",
		strike: 100,
		premium: 5,
		quantity: 1,
		cashAmount: 1,
	},
];

export default function Visualize() {
	const [legs, setLegs] = useState<Leg[]>(defaultLegs);
	const [strategy, setStrategy] = useState<StrategyKey>("long-call");
	const [spotMin, setSpotMin] = useState(60);
	const [spotMax, setSpotMax] = useState(140);
	const [focusAggregate, setFocusAggregate] = useState(true);

	const grid = useMemo(() => buildGrid(spotMin, spotMax, 200), [spotMin, spotMax]);
	const metrics = useMemo(() => computeMetrics(legs, grid), [legs, grid]);

	/* ---- strategy picker ---- */
	const applyStrategy = useCallback((key: StrategyKey) => {
		setStrategy(key);
		if (key === "custom") return;
		const def = STRATEGIES[key];
		const atm = 100;
		setLegs(def.buildLegs(atm).map((l) => ({ ...l, id: makeId() })));
	}, []);

	/* ---- leg CRUD ---- */
	const addLeg = useCallback(() => {
		setStrategy("custom");
		setLegs((prev) => [
			...prev,
			{
				id: makeId(),
				label: "New Leg",
				type: "call" as LegType,
				side: "long",
				strike: 100,
				premium: 5,
				quantity: 1,
				cashAmount: 1,
			},
		]);
	}, []);

	const updateLeg = useCallback((updated: Leg) => {
		setStrategy("custom");
		setLegs((prev) => prev.map((l) => (l.id === updated.id ? updated : l)));
	}, []);

	const removeLeg = useCallback((id: string) => {
		setStrategy("custom");
		setLegs((prev) => prev.filter((l) => l.id !== id));
	}, []);

	/* ---- chart series ---- */
	const aggregateSeries: PnlSeries = useMemo(
		() => ({
			id: "aggregate",
			label: "Total P&L",
			color: palette[0],
			points: grid.map((s) => ({ x: s, y: portfolioPayoff(s, legs) })),
		}),
		[grid, legs],
	);

	const legSeries: PnlSeries[] = useMemo(
		() =>
			legs.map((leg, idx) => ({
				id: leg.id,
				label: leg.label,
				color: palette[(idx + 1) % palette.length],
				points: grid.map((s) => ({ x: s, y: legPayoff(s, leg) })),
				opacity: focusAggregate ? 0.18 : 1,
			})),
		[grid, legs, focusAggregate],
	);

	const chartSeries = useMemo<PnlSeries[]>(() => {
		if (legs.length <= 1) return [aggregateSeries];
		return [aggregateSeries, ...legSeries];
	}, [aggregateSeries, legSeries, legs.length]);

	return (
		<>
			<section className="hero">
				<div>
					<h1>Strategy builder.</h1>
					<p>
						Pick a template or build your own multi-leg portfolio.
						Every change updates the payoff diagram instantly.
					</p>
				</div>
			</section>

			<section className="card">
				{/* strategy pills */}
				<div className="category-bar">
					{STRATEGY_KEYS.map((key) => (
						<button
							key={key}
							className={`pill${strategy === key ? " active" : ""}`}
							onClick={() => applyStrategy(key)}
							title={STRATEGIES[key].desc}
						>
							{STRATEGIES[key].label}
						</button>
					))}
				</div>

				{/* chart */}
				<div className="chart-wrap" style={{ marginTop: 8 }}>
					<PnlChart
						title="Payoff at expiry"
						series={chartSeries}
						showLegend={legs.length > 1}
						showFill
						breakevens={metrics.breakevens}
						chartHeight={200}
					/>
				</div>

				{/* legs */}
				<h3 style={{ marginTop: 24, marginBottom: 12 }}>Legs</h3>
				<div className="legs-grid">
					{legs.map((leg, idx) => (
						<LegCard
							key={leg.id}
							leg={leg}
							color={
								legs.length > 1
									? palette[(idx + 1) % palette.length]
									: palette[0]
							}
							onUpdate={updateLeg}
							onRemove={() => removeLeg(leg.id)}
						/>
					))}
					<button className="leg-card leg-card-add" onClick={addLeg}>
						<span className="leg-card-add-icon">+</span>
						<span>Add leg</span>
					</button>
				</div>

				{/* settings row */}
				<div className="viz-settings">
					<label className="field">
						Min spot
						<input
							type="number"
							value={spotMin}
							onChange={(e) => setSpotMin(Number(e.target.value))}
						/>
					</label>
					<label className="field">
						Max spot
						<input
							type="number"
							value={spotMax}
							onChange={(e) => setSpotMax(Number(e.target.value))}
						/>
					</label>
					{legs.length > 1 && (
						<label
							className="field"
							style={{
								display: "flex",
								flexDirection: "row",
								alignItems: "center",
								gap: 8,
							}}
						>
							<input
								type="checkbox"
								checked={focusAggregate}
								onChange={(e) => setFocusAggregate(e.target.checked)}
							/>
							Focus aggregate
						</label>
					)}
				</div>

				{/* metrics */}
				<MetricsPanel
					maxProfit={metrics.maxProfit}
					maxLoss={metrics.maxLoss}
					breakevens={metrics.breakevens}
					netPremium={metrics.netPremium}
					hasLegs={legs.length > 0}
				/>
			</section>
		</>
	);
}
