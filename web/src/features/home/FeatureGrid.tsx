import { Link } from "@tanstack/react-router";
import {
	BookOpen,
	Braces,
	Briefcase,
	Calculator,
	ChartCandlestick,
	History,
	Layers,
	ShieldAlert,
	Waves,
	type LucideIcon,
} from "lucide-react";

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
		text: "Optimise a portfolio on one window, then see how it fares on the next.",
	},
	{
		to: "/products",
		title: "Product reference",
		icon: BookOpen,
		text: "Each product documented: payoff, model assumptions, and the paper or textbook it comes from.",
	},
];

export function FeatureGrid() {
	return (
		<ul className="grid grid-cols-1 gap-3 sm:grid-cols-2 lg:grid-cols-4">
			{FEATURES.map((f) => (
				<li key={f.to}>
					<Link
						to={f.to}
						className="group flex h-full flex-col gap-2 rounded-md border border-hairline bg-surface p-4 transition-colors hover:border-accent/60"
					>
						<span className="flex items-center gap-2 text-sm font-semibold text-ink">
							<f.icon className="size-4 text-accent" aria-hidden="true" />
							{f.title}
						</span>
						<span className="text-xs leading-relaxed text-ink-secondary">
							{f.text}
						</span>
					</Link>
				</li>
			))}
		</ul>
	);
}
