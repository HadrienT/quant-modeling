import { useCallback, useMemo, useState } from "react";
import { useNavigate, useSearch } from "@tanstack/react-router";
import { Copy, Plus } from "lucide-react";
import {
	type Leg,
	type MarketInputs,
	evaluateStrategy,
	preset,
} from "@/shared/payoff";
import { Button, Input, Label, toast } from "@/shared/ui";
import { LegCard } from "./LegCard";
import { StrategyOutcome } from "./StrategyOutcome";

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

	// Which leg is currently highlighted — set from either the leg card or the
	// dashed payoff line, so hovering one lights up the other.
	const [hoveredLeg, setHoveredLeg] = useState<number | null>(null);

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
							highlighted={hoveredLeg === i}
							onHover={(over) => setHoveredLeg(over ? i : null)}
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

				<StrategyOutcome
					result={result}
					mkt={state.mkt}
					legs={state.legs}
					hoveredLeg={hoveredLeg}
					onLegHover={setHoveredLeg}
				/>
			</div>
		</div>
	);
}
