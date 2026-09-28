import { Suspense, lazy } from "react";
import { Link } from "@tanstack/react-router";
import { ArrowRight } from "lucide-react";
import { SITE } from "@/shared/config";
import { Disclaimer, buttonVariants } from "@/shared/ui";
import { ChartSkeleton } from "@/shared/ui/states";
import { FeatureGrid } from "./FeatureGrid";

const HeroSurface = lazy(() => import("./HeroSurface"));

const STACK = [
	{ name: "C++20 core", text: "payoffs, models, engines, pricers" },
	{ name: "pybind11", text: "the library as a Python module" },
	{ name: "FastAPI", text: "pricing, market data, portfolios" },
	{ name: "React", text: "this front end, typed from the API schema" },
];

const DEPTH = [
	"Each product documented with the paper or textbook it comes from.",
	"Greeks by adjoint differentiation (AAD) in the library, not only by bumping.",
	"Monte Carlo on two GPUs, with results identical bit for bit.",
	"Every valuation logged with its inputs, and replayable exactly.",
];

/** The front door: what the site is, why it is different, where to start. */
export default function HomePage() {
	return (
		<div className="mx-auto flex max-w-6xl flex-col gap-14 py-4 sm:py-8">
			<section className="grid grid-cols-1 items-center gap-8 lg:grid-cols-[minmax(0,5fr)_minmax(0,6fr)]">
				<div className="flex flex-col gap-5">
					<p className="text-xs font-medium tracking-wide text-accent uppercase">
						Open-source derivatives pricing
					</p>
					<h1 className="text-3xl leading-tight font-semibold text-ink sm:text-4xl">
						From a C++ pricing library to your browser.
					</h1>
					<p className="text-base leading-relaxed text-ink-secondary">
						Analytic, tree, PDE and Monte Carlo engines, calibrated local and
						stochastic volatility, a payoff scripting language and GPU Monte
						Carlo — each one usable here, with the model and its inputs shown
						next to every number.
					</p>
					<div className="flex flex-wrap gap-2">
						<Link to="/price" className={buttonVariants({ size: "lg" })}>
							Price an option
							<ArrowRight className="size-4" />
						</Link>
						<Link
							to="/scripting"
							className={buttonVariants({ size: "lg", variant: "secondary" })}
						>
							Script a payoff
						</Link>
					</div>
					<p className="text-xs text-ink-muted">
						No account needed. Signing in only keeps portfolios on the server.
					</p>
				</div>
				<Suspense
					fallback={<ChartSkeleton className="!aspect-auto h-[380px]" />}
				>
					<HeroSurface />
				</Suspense>
			</section>

			<section className="flex flex-col gap-4">
				<h2 className="text-lg font-semibold text-ink">What you can do</h2>
				<FeatureGrid />
			</section>

			<section className="grid grid-cols-1 gap-8 lg:grid-cols-2">
				<div className="flex flex-col gap-3">
					<h2 className="text-lg font-semibold text-ink">Depth over breadth</h2>
					<ul className="flex flex-col gap-2 text-sm text-ink-secondary">
						{DEPTH.map((d) => (
							<li key={d} className="flex gap-2">
								<span
									className="mt-2 size-1.5 shrink-0 rounded-full bg-accent"
									aria-hidden="true"
								/>
								{d}
							</li>
						))}
					</ul>
				</div>
				<div className="flex flex-col gap-3">
					<h2 className="text-lg font-semibold text-ink">
						One stack, end to end
					</h2>
					<ol className="flex flex-col gap-2">
						{STACK.map((s, i) => (
							<li
								key={s.name}
								className="flex items-baseline gap-3 rounded-md border border-hairline bg-surface px-3 py-2 text-sm"
							>
								<span className="font-mono text-2xs text-ink-muted">
									{i + 1}
								</span>
								<span className="font-semibold text-ink">{s.name}</span>
								<span className="text-xs text-ink-secondary">{s.text}</span>
							</li>
						))}
					</ol>
					<p className="text-xs text-ink-secondary">
						The code is open source under the MIT licence:{" "}
						<a
							className="text-accent hover:underline"
							href={SITE.repository}
							target="_blank"
							rel="noreferrer"
						>
							read it on GitHub
						</a>
						, or see{" "}
						<Link to="/about" className="text-accent hover:underline">
							how it is built
						</Link>
						.
					</p>
				</div>
			</section>

			<Disclaimer />
		</div>
	);
}
