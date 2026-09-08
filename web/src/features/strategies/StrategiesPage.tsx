import { useCallback, useMemo } from "react";
import { useNavigate, useSearch } from "@tanstack/react-router";
import { Copy, Plus, Trash2 } from "lucide-react";
import {
	type Leg,
	type MarketInputs,
	evaluateStrategy,
	preset,
} from "@/shared/payoff";
import { Button, Input, Label, cn, toast } from "@/shared/ui";
import { Metric, MetricRow, NumberCell } from "@/shared/ui";
import { GreekProfileChart, PayoffChart } from "@/shared/viz";

/**
 * Strategy visualiser & home page (WP 10). Build a multi-leg position, see its
 * payoff, value at t and greeks instantly. Whole strategy lives in the URL.
 */

const PRESETS = [
	"straddle",
	"strangle",
	"call-spread",
	"put-spread",
	"butterfly",
	"condor",
	"collar",
	"risk-reversal",
	"covered-call",
	"protective-put",
	"calendar-spread",
];

type State = { mkt: MarketInputs; legs: Leg[] };

const DEFAULT: State = {
	mkt: { spot: 100, rate: 0.04, dividend: 0, vol: 0.2 },
	legs: preset("call-spread", 100) ?? [],
};

function encode(s: State) {
	return btoa(unescape(encodeURIComponent(JSON.stringify(s))))
		.replace(/\+/g, "-")
		.replace(/\//g, "_")
		.replace(/=+$/, "");
}
function decode(s: string | undefined): State {
	if (!s) return DEFAULT;
	try {
		return JSON.parse(
			decodeURIComponent(escape(atob(s.replace(/-/g, "+").replace(/_/g, "/")))),
		) as State;
	} catch {
		return DEFAULT;
	}
}

export default function StrategiesPage() {
	const search = useSearch({ from: "/visualize" });
	const navigate = useNavigate({ from: "/visualize" });
	const state = useMemo(() => decode(search.s), [search.s]);

	const update = useCallback(
		(next: State) => {
			navigate({ search: () => ({ s: encode(next) }) });
		},
		[navigate],
	);

	const result = useMemo(
		() => evaluateStrategy(state.legs, state.mkt),
		[state],
	);

	const probProfit = useMemo(() => {
		// fraction of the plotted spot grid (equal-weighted proxy) with payoff > 0
		const wins = result.atMaturity.filter((v) => v > 0).length;
		return wins / result.atMaturity.length;
	}, [result]);

	const rr =
		typeof result.maxGain === "number" && typeof result.maxLoss === "number"
			? Math.abs(result.maxGain / result.maxLoss)
			: null;

	return (
		<div className="mx-auto flex max-w-6xl flex-col gap-5">
			<header className="flex items-center justify-between">
				<h1 className="text-lg font-semibold text-ink">Strategy visualiser</h1>
				<div className="flex items-center gap-2">
					<select
						className="h-8 rounded-sm border border-hairline bg-surface px-2 text-xs text-ink"
						value=""
						onChange={(e) => {
							const legs = preset(e.target.value, state.mkt.spot);
							if (legs) update({ ...state, legs });
						}}
					>
						<option value="">Load a preset…</option>
						{PRESETS.map((p) => (
							<option key={p} value={p}>
								{p}
							</option>
						))}
					</select>
					<Button
						size="sm"
						variant="secondary"
						onClick={() => {
							void navigator.clipboard?.writeText(window.location.href);
							toast.success("Strategy link copied");
						}}
					>
						<Copy className="size-3.5" /> Share
					</Button>
				</div>
			</header>

			<div className="grid gap-3 rounded-md border border-hairline bg-surface p-3 sm:grid-cols-4">
				{(
					[
						["spot", "Spot", 1],
						["vol", "Volatility (%)", 100],
						["rate", "Rate (%)", 100],
						["dividend", "Dividend (%)", 100],
					] as const
				).map(([k, label, mul]) => (
					<div key={k} className="flex flex-col gap-1">
						<Label htmlFor={`m-${k}`}>{label}</Label>
						<Input
							id={`m-${k}`}
							type="number"
							value={state.mkt[k] * mul}
							onChange={(e) =>
								update({
									...state,
									mkt: { ...state.mkt, [k]: Number(e.target.value) / mul },
								})
							}
						/>
					</div>
				))}
			</div>

			<div className="grid gap-4 lg:grid-cols-[320px_1fr]">
				<div className="flex flex-col gap-2">
					{state.legs.map((leg, i) => (
						<LegCard
							key={leg.id}
							leg={leg}
							onChange={(next) =>
								update({
									...state,
									legs: state.legs.map((l, j) => (j === i ? next : l)),
								})
							}
							onRemove={() =>
								update({
									...state,
									legs: state.legs.filter((_, j) => j !== i),
								})
							}
						/>
					))}
					<Button
						size="sm"
						variant="ghost"
						onClick={() =>
							update({
								...state,
								legs: [
									...state.legs,
									{
										id: Math.random().toString(36).slice(2, 8),
										kind: "call",
										direction: "long",
										quantity: 1,
										strike: Math.round(state.mkt.spot),
										maturity: 0.25,
										premium: state.mkt.spot * 0.03,
									},
								],
							})
						}
					>
						<Plus className="size-3.5" /> Add leg
					</Button>
				</div>

				<div className="flex flex-col gap-4">
					<MetricRow>
						<Metric
							label="Net cost"
							value={
								<NumberCell value={result.netCost} magnitude="price" signed />
							}
							footnote={result.netCost > 0 ? "debit" : "credit"}
						/>
						<Metric
							label="Max gain"
							value={
								result.maxGain === "unbounded" ? (
									"unbounded"
								) : (
									<NumberCell value={result.maxGain} magnitude="price" />
								)
							}
						/>
						<Metric
							label="Max loss"
							value={
								result.maxLoss === "unbounded" ? (
									"unbounded"
								) : (
									<NumberCell value={result.maxLoss} magnitude="price" />
								)
							}
						/>
						<Metric
							label="Breakevens"
							value={
								result.breakevens.length
									? result.breakevens.map((b) => b.toFixed(1)).join(" · ")
									: "—"
							}
						/>
						<Metric
							label="Prob. of profit"
							value={`${(probProfit * 100).toFixed(0)}%`}
							footnote="risk-neutral proxy, not real-world"
						/>
					</MetricRow>
					{rr != null && (
						<p className="text-2xs text-ink-muted">
							Risk / reward ≈ {rr.toFixed(2)}
						</p>
					)}

					<PayoffChart
						data={{
							spot: result.spot,
							atMaturity: result.atMaturity,
							atT: result.atT,
							legs: result.legs,
							breakevens: result.breakevens,
							currentSpot: state.mkt.spot,
							strikes: state.legs
								.filter((l) => l.kind !== "underlying")
								.map((l) => l.strike),
						}}
					/>
					<GreekProfileChart
						currentSpot={state.mkt.spot}
						profiles={result.greeks}
					/>
				</div>
			</div>
		</div>
	);
}

function LegCard({
	leg,
	onChange,
	onRemove,
}: {
	leg: Leg;
	onChange: (l: Leg) => void;
	onRemove: () => void;
}) {
	return (
		<div className="flex flex-col gap-2 rounded-md border border-hairline bg-surface p-2.5">
			<div className="flex items-center gap-1.5">
				<button
					type="button"
					className={cn(
						"rounded-xs px-1.5 py-0.5 text-2xs font-medium",
						leg.direction === "long"
							? "bg-pnl-up/15 text-pnl-up"
							: "bg-pnl-down/15 text-pnl-down",
					)}
					onClick={() =>
						onChange({
							...leg,
							direction: leg.direction === "long" ? "short" : "long",
						})
					}
				>
					{leg.direction}
				</button>
				<select
					className="h-7 rounded-sm border border-hairline bg-surface px-1 text-xs text-ink"
					value={leg.kind}
					onChange={(e) =>
						onChange({ ...leg, kind: e.target.value as Leg["kind"] })
					}
				>
					{["call", "put", "underlying", "forward"].map((k) => (
						<option key={k}>{k}</option>
					))}
				</select>
				<button
					type="button"
					aria-label="Remove leg"
					className="ml-auto text-ink-muted hover:text-critical"
					onClick={onRemove}
				>
					<Trash2 className="size-3.5" />
				</button>
			</div>
			<div className="grid grid-cols-2 gap-1.5 text-xs">
				<Field
					label="Qty"
					value={leg.quantity}
					onChange={(v) => onChange({ ...leg, quantity: v })}
				/>
				{leg.kind !== "underlying" && (
					<Field
						label="Strike"
						value={leg.strike}
						onChange={(v) => onChange({ ...leg, strike: v })}
					/>
				)}
				<Field
					label="Maturity (y)"
					value={leg.maturity}
					step={0.05}
					onChange={(v) => onChange({ ...leg, maturity: v })}
				/>
				<Field
					label="Premium"
					value={leg.premium}
					step={0.1}
					onChange={(v) => onChange({ ...leg, premium: v })}
				/>
			</div>
		</div>
	);
}

function Field({
	label,
	value,
	step,
	onChange,
}: {
	label: string;
	value: number;
	step?: number;
	onChange: (v: number) => void;
}) {
	return (
		<label className="flex flex-col gap-0.5">
			<span className="text-2xs text-ink-muted">{label}</span>
			<Input
				type="number"
				step={step}
				value={value}
				onChange={(e) => onChange(Number(e.target.value))}
				className="h-7"
			/>
		</label>
	);
}
