import { CATEGORY_ORDER, SCRIPT_LIBRARY, type LibraryProduct } from "./library";

/** The product library, grouped by family, and what the chosen product is:
 * a plain-words summary, how many underlyings it reads, and its sources. */
export function LibraryPicker(props: {
	product: LibraryProduct;
	onPick: (p: LibraryProduct) => void;
}) {
	const p = props.product;
	return (
		<div className="flex flex-col gap-2">
			<label className="flex flex-col gap-1">
				<span className="text-2xs text-ink-muted uppercase">
					Product library ({SCRIPT_LIBRARY.length} scripts)
				</span>
				<select
					className="rounded border border-hairline bg-surface px-2 py-1 text-sm"
					value={p.slug}
					onChange={(e) => {
						const next = SCRIPT_LIBRARY.find((x) => x.slug === e.target.value);
						if (next) props.onPick(next);
					}}
				>
					{CATEGORY_ORDER.map((category) => (
						<optgroup key={category} label={category}>
							{SCRIPT_LIBRARY.filter((x) => x.category === category).map(
								(x) => (
									<option key={x.slug} value={x.slug}>
										{x.title}
										{x.underlyings > 1 ? ` (${x.underlyings} underlyings)` : ""}
									</option>
								),
							)}
						</optgroup>
					))}
				</select>
			</label>
			<div className="rounded border border-hairline bg-surface-raised p-3 text-2xs">
				<p className="text-sm text-ink">{p.summary}</p>
				<p className="mt-1 text-ink-muted">
					{p.category} ·{" "}
					{p.underlyings === 1
						? "one underlying"
						: `${p.underlyings} underlyings: spot(0) to spot(${p.underlyings - 1})`}
					. Dates are moved to start just after the valuation date.
				</p>
				<ul className="mt-1 list-disc pl-4 text-ink-secondary">
					{p.sources.map((s) => (
						<li key={s}>{s}</li>
					))}
				</ul>
			</div>
		</div>
	);
}
