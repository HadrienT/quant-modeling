import { Suspense, lazy } from "react";
import { Link } from "@tanstack/react-router";
import { ArrowRight } from "lucide-react";
import { useNavPages } from "@/shared/session";
import { Disclaimer, buttonVariants } from "@/shared/ui";
import { ChartSkeleton } from "@/shared/ui/states";
import { FeatureGrid } from "./FeatureGrid";
import { StartingPoints } from "./StartingPoints";

const HeroSurface = lazy(() => import("./HeroSurface"));

/**
 * The front door: what the site is, why it is different, where to start.
 * Every link to a page follows the admin panel, like the navigation: a page
 * hidden from visitors is not offered to them here either.
 */
export default function HomePage() {
	const { isShown } = useNavPages();
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
					{(isShown("/price") || isShown("/scripting")) && (
						<div className="flex flex-wrap gap-2">
							{isShown("/price") && (
								<Link to="/price" className={buttonVariants({ size: "lg" })}>
									Price an option
									<ArrowRight className="size-4" />
								</Link>
							)}
							{isShown("/scripting") && (
								<Link
									to="/scripting"
									className={buttonVariants({
										size: "lg",
										variant: isShown("/price") ? "secondary" : "primary",
									})}
								>
									Script a payoff
									{!isShown("/price") && <ArrowRight className="size-4" />}
								</Link>
							)}
						</div>
					)}
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

			<StartingPoints />

			<FeatureGrid />

			<Disclaimer />
		</div>
	);
}
