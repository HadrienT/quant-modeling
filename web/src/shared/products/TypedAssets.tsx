import { Input } from "@/shared/ui";

const DEFAULT = ["100", "20", "0"];

/**
 * Typed underlyings of a multi-asset product, one row per spot(i): spot,
 * vol % and dividend yield %, kept as "spot,vol,div;..." in the form (and
 * the page's link).
 */
export function TypedAssets({
	value,
	count,
	onChange,
}: {
	value: string;
	count: number;
	onChange: (v: string) => void;
}) {
	const rows = Array.from({ length: count }, (_, i) => {
		const cells = (value.split(";")[i] ?? "").split(",");
		return DEFAULT.map((d, k) => cells[k] ?? d);
	});
	const set = (i: number, k: number, x: string) => {
		const next = rows.map((r) => [...r]);
		next[i]![k] = x;
		onChange(next.map((r) => r.join(",")).join(";"));
	};
	return (
		<table className="w-full text-sm">
			<thead>
				<tr className="text-left text-2xs text-ink-muted">
					<th className="pr-2 font-normal" />
					<th className="pr-2 font-normal">Spot</th>
					<th className="pr-2 font-normal">Vol %</th>
					<th className="font-normal">Dividend %</th>
				</tr>
			</thead>
			<tbody>
				{rows.map((r, i) => (
					<tr key={i}>
						<td className="pr-2 font-mono text-2xs text-ink-muted">
							spot({i})
						</td>
						{r.map((cell, k) => (
							<td key={k} className="pr-2 pb-1">
								<Input
									type="number"
									step="any"
									aria-label={`spot(${i}) ${["spot", "vol", "dividend"][k]}`}
									value={cell}
									onChange={(e) => set(i, k, e.target.value)}
								/>
							</td>
						))}
					</tr>
				))}
			</tbody>
		</table>
	);
}
