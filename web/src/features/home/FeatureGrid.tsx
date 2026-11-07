import { Link } from "@tanstack/react-router";
import {
	BookOpen,
	Braces,
	Briefcase,
	Calculator,
	ChartCandlestick,
	EyeOff,
	History,
	Layers,
	ShieldAlert,
	Waves,
	type LucideIcon,
} from "lucide-react";
import { useNavPages } from "@/shared/session";
import { cn } from "@/shared/ui";

type Feature = {
	to:
		| "/visualize"
		| "/price"
		| "/scripting"
		| "/market"
		| "/xva"
		| "/simulation"
		| "/portfolio"
		| "/backtest"
		| "/products";
	title: string;
	text: string;
	icon: LucideIcon;
};

const FEATURES: Feature[] = [
	{
		to: "/price",
		title: "Pricing",
		icon: Calculator,
		text: "The product catalogue under analytic, tree, PDE and Monte Carlo engines, on CPU or GPU, with greeks, convergence diagnostics and model comparison.",
	},
	{
		to: "/scripting",
		title: "Scripting",
		icon: Braces,
		text: "Write a payoff in a small language, priced on the CPU or compiled for the GPU. An assistant drafts scripts, which the real parser checks first.",
	},
	{
		to: "/market",
		title: "Market",
		icon: ChartCandlestick,
		text: "Prices, rate curves, FX, and implied and local volatility surfaces built from option chains in the site's own database.",
	},
	{
		to: "/xva",
		title: "xVA",
		icon: ShieldAlert,
		text: "The counterparty risk of a netting set of rate trades: simulated exposure, CVA, DVA, FVA, margin and capital, with their sensitivities by adjoint differentiation.",
	},
	{
		to: "/visualize",
		title: "Strategies",
		icon: Layers,
		text: "Assemble an option strategy leg by leg and read its payoff and greeks at a glance.",
	},
	{
		to: "/simulation",
		title: "Simulation",
		icon: Waves,
		text: "Simulate Black–Scholes, SABR, local vol, Heston and SLV paths, each calibrated to a ticker's option surface.",
	},
	{
		to: "/portfolio",
		title: "Portfolio",
		icon: Briefcase,
		text: "Book trades, mark derivatives every day, and follow P&L, risk, stress tests and value at risk.",
	},
	{
		to: "/backtest",
		title: "Backtest",
		icon: History,
		text: "Run an allocation strategy on real prices, net of costs, with a confidence interval on every figure and tests against overfitting.",
	},
	{
		to: "/products",
		title: "Product reference",
		icon: BookOpen,
		text: "Each product documented: payoff, model assumptions, and the paper or textbook it comes from.",
	},
];

/**
 * The tiles follow the admin panel like the navigation does: a page hidden
 * from visitors has no tile for them, and an administrator sees its tile
 * faded and labelled. Nothing renders when every page is hidden.
 */
export function FeatureGrid() {
	const { isShown, isHidden } = useNavPages();
	const features = FEATURES.filter((f) => isShown(f.to));
	if (features.length === 0) return null;
	return (
		<section className="flex flex-col gap-4">
			<h2 className="text-lg font-semibold text-ink">What you can do</h2>
			<ul className="grid grid-cols-1 gap-3 sm:grid-cols-2 lg:grid-cols-4">
				{features.map((f) => (
					<FeatureTile key={f.to} feature={f} hidden={isHidden(f.to)} />
				))}
			</ul>
		</section>
	);
}

function FeatureTile({
	feature: f,
	hidden,
}: {
	feature: Feature;
	hidden: boolean;
}) {
	return (
		<li>
			<Link
				to={f.to}
				title={hidden ? "Hidden from visitors" : undefined}
				className={cn(
					"group flex h-full flex-col gap-2 rounded-md border border-hairline bg-surface p-4 transition-colors hover:border-accent/60",
					hidden && "border-dashed opacity-60",
				)}
			>
				<span className="flex items-center gap-2 text-sm font-semibold text-ink">
					<f.icon className="size-4 text-accent" aria-hidden="true" />
					{f.title}
					{hidden && (
						<span className="ml-auto flex items-center gap-1 text-2xs font-normal text-ink-muted">
							<EyeOff className="size-3" aria-hidden="true" />
							Hidden from visitors
						</span>
					)}
				</span>
				<span className="text-xs leading-relaxed text-ink-secondary">
					{f.text}
				</span>
			</Link>
		</li>
	);
}
