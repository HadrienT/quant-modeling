type HoverItem = {
	key?: string;
	label: string;
	color: string;
	value: string;
};

type ChartHoverCardProps = {
	title: string;
	items: HoverItem[];
	titleClassName?: string;
	valueClassName?: string;
};

export default function ChartHoverCard({
	title,
	items,
	titleClassName,
	valueClassName
}: ChartHoverCardProps) {
	return (
		<div className="pnl-hover-card">
			<div className={titleClassName ?? "pnl-hover-title"}>{title}</div>
			{items.map((item) => (
				<div key={item.key ?? item.label} className="pnl-hover-row">
					<span className="pnl-hover-swatch" style={{ background: item.color }} />
					<span className="pnl-hover-label">{item.label}</span>
					<span className={valueClassName}>{item.value}</span>
				</div>
			))}
		</div>
	);
}
