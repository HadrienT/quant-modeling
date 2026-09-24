import type { PositionMark, PositionModel } from "@/shared/api";
import { Badge } from "@/shared/ui";

type Param = PositionModel["params"][number];

const TONE = {
	observed: "good",
	calibrated: "accent",
	contract: "neutral",
	stale: "warning",
	proxied: "warning",
	default: "serious",
} as const;

/** Rates, vols, yields and correlations read as percentages. */
const PERCENT = /vol|rate|dividend|correlation/i;

function show(p: Param): string {
	if (p.text != null) return p.text;
	if (p.value == null) return "—";
	if (PERCENT.test(p.name) && !/conversion|skew/.test(p.name)) {
		return `${(p.value * 100).toFixed(2)} %`;
	}
	if (Number.isInteger(p.value)) return p.value.toLocaleString("en-US");
	return p.value.toPrecision(5);
}

/**
 * What priced a position: the model, its engine, why that model for that
 * product, and every parameter with its value and where it came from.
 */
export function ModelDetails({ position: p }: { position: PositionMark }) {
	const m = p.model;
	return (
		<>
			{m ? (
				<div className="flex flex-col gap-2">
					<div>
						<p className="text-sm font-semibold text-ink">{m.model}</p>
						<p className="text-2xs text-ink-muted">{m.engine}</p>
					</div>
					<p className="text-xs leading-relaxed text-ink-secondary">{m.why}</p>
					<table className="w-full text-2xs">
						<tbody>
							{m.params.map((x) => (
								<tr key={x.name} className="border-t border-hairline align-top">
									<td className="py-1 pr-2 text-ink-secondary">
										{x.name}
										{x.source && (
											<span className="block text-ink-muted">{x.source}</span>
										)}
									</td>
									<td className="py-1 pr-2 text-right font-mono whitespace-nowrap text-ink">
										{show(x)}
									</td>
									<td className="py-1">
										<Badge tone={TONE[x.status]}>{x.status}</Badge>
									</td>
								</tr>
							))}
						</tbody>
					</table>
					{m.std_error != null && (
						<p className="text-2xs text-ink-muted">
							Monte-Carlo standard error of the unit mark: ±
							{m.std_error.toPrecision(3)} {p.currency}
						</p>
					)}
				</div>
			) : (
				<p className="text-xs text-ink-secondary">
					{p.kind === "equity"
						? "No model: marked at the stored close of the day, in its listing currency."
						: (p.note ?? "Not valued on this date.")}
				</p>
			)}
		</>
	);
}
