import { useMemo } from "react";
import { Link } from "@tanstack/react-router";
import { SurfaceView } from "@/shared/viz";
import { SSVI, ssviGrid } from "./ssvi";

/**
 * The surface engine the Market page uses, on a model surface computed in the
 * browser: no API call per visitor, and labelled for what it is. Lazy-loaded
 * with three.js so the text of the page paints first.
 */
export default function HeroSurface() {
	const grid = useMemo(ssviGrid, []);
	return (
		<figure className="flex flex-col gap-2">
			<SurfaceView grid={grid} title="SSVI implied volatility" height={380} />
			<figcaption className="text-2xs leading-relaxed text-ink-muted">
				A model surface, not market data: SSVI (Gatheral &amp; Jacquier, 2014)
				with {SSVI.atmVol * 100}% ATM vol, ρ = {SSVI.rho}, η = {SSVI.eta}, γ ={" "}
				{SSVI.gamma}, inside the paper&apos;s no-arbitrage bound. Surfaces built
				from real option chains are on{" "}
				<Link
					to="/market"
					search={{ tab: "vol" }}
					className="text-ink-secondary underline-offset-2 hover:underline"
				>
					Market
				</Link>
				.
			</figcaption>
		</figure>
	);
}
