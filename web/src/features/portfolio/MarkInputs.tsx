import type { PositionMark } from "@/shared/api";
import { Badge, NumberCell } from "@/shared/ui";
import { MEANING, STATUS_TONE } from "./markStatus";

const RATE_LIKE = /rate|dividend|vol/;

/** Every input of a position's mark: value, date observed, and status. */
export function MarkInputs({ position: p }: { position: PositionMark }) {
	return (
		<div className="flex flex-col gap-2">
			{p.note && <p className="text-xs text-warning">{p.note}</p>}
			<table className="text-xs">
				<tbody>
					{p.inputs.map((i) => (
						<tr key={i.name}>
							<td className="pr-4 text-ink-secondary">{i.name}</td>
							<td className="pr-4 text-right">
								<NumberCell
									value={i.value}
									magnitude={RATE_LIKE.test(i.name) ? "rate" : "plain"}
									className="text-xs"
								/>
							</td>
							<td className="pr-4 text-ink-muted">{i.as_of ?? ""}</td>
							<td className="pr-4">
								<Badge tone={STATUS_TONE[i.status]}>{i.status}</Badge>
							</td>
							<td className="text-2xs text-ink-muted">{MEANING[i.status]}</td>
						</tr>
					))}
				</tbody>
			</table>
			{p.kind === "derivative" && p.mark != null && (
				<p className="text-2xs text-ink-muted">
					Unit mark {p.mark.toFixed(4)} {p.currency}
					{p.fx_rate != null &&
						p.fx_rate !== 1 &&
						`, FX ${p.fx_rate.toFixed(4)}`}
					{Object.entries(p.greeks)
						.filter(([, v]) => v != null)
						.map(([k, v]) => ` · ${k} ${Number(v).toPrecision(4)}`)
						.join("")}
				</p>
			)}
		</div>
	);
}
