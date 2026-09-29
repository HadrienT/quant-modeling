/**
 * A row of mutually exclusive buttons (aria-pressed), for the small choices
 * data pages offer: a currency, a forward period, a frequency.
 */
export function Segmented<T extends string | number>({
	options,
	value,
	onChange,
	label,
}: {
	options: { value: T; label: string; title?: string }[];
	value: T;
	onChange: (v: T) => void;
	label: string;
}) {
	return (
		<div role="group" aria-label={label} className="flex flex-wrap gap-1">
			{options.map((o) => (
				<button
					key={String(o.value)}
					type="button"
					aria-pressed={o.value === value}
					title={o.title}
					onClick={() => onChange(o.value)}
					className={
						"rounded-sm border px-2 py-1 text-xs " +
						(o.value === value
							? "border-accent text-ink"
							: "border-hairline text-ink-secondary")
					}
				>
					{o.label}
				</button>
			))}
		</div>
	);
}
