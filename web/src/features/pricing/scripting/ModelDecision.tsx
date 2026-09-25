import type { ReactNode } from "react";
import type { ModelChoice } from "@/shared/api";
import { formatDuration, formatNumber } from "@/shared/format";
import { Badge } from "@/shared/ui";
import { MODEL_LABELS } from "./modelLabels";

type Calibration = NonNullable<ModelChoice["calibration"]>;

function Row({ label, children }: { label: string; children: ReactNode }) {
	return (
		<>
			<dt className="text-ink-muted">{label}</dt>
			<dd className="font-mono text-ink">{children}</dd>
		</>
	);
}

/** A variance shown with the volatility it stands for. */
function variance(v: number) {
	return `${formatNumber(v, "greek")} (${formatNumber(Math.sqrt(v), "vol")} vol)`;
}

function HestonRows({ h }: { h: NonNullable<Calibration["heston"]> }) {
	return (
		<>
			<Row label="v₀ initial variance">{variance(h.v0)}</Row>
			<Row label="κ mean reversion">{formatNumber(h.kappa, "greek")}</Row>
			<Row label="θ long-run variance">{variance(h.theta)}</Row>
			<Row label="ξ vol of vol">{formatNumber(h.xi, "greek")}</Row>
			<Row label="ρ spot / vol correlation">{formatNumber(h.rho, "plain")}</Row>
			<Row label="Fit to the surface">
				RMSE {formatNumber(h.iv_rmse, "vol")}, worst{" "}
				{formatNumber(h.iv_worst, "vol")} of implied vol, over {h.n_quotes}{" "}
				points on {h.n_maturities} maturities
			</Row>
			<Row label="Feller (2κθ > ξ²)">
				{h.feller ? "holds" : "does not hold (variance can reach 0)"}
			</Row>
		</>
	);
}

function LeverageRows({ l }: { l: NonNullable<Calibration["leverage"]> }) {
	return (
		<>
			<Row label="Leverage L(K, T)">
				{formatNumber(l.min, "plain")} to {formatNumber(l.max, "plain")}
			</Row>
			<Row label="Held at its floor or cap">
				{formatNumber(l.clamped_share, "rate")} of the grid (surface not matched
				there)
			</Row>
			<Row label="Particles">{formatNumber(l.n_particles, "integer")}</Row>
		</>
	);
}

/** Which model priced the script, why, and what was calibrated for it —
 * every number here is a reported calibration output, not an estimate. */
export function ModelDecision({ choice }: { choice: ModelChoice }) {
	const cal = choice.calibration;
	const auto = choice.requested === "auto";
	return (
		<section
			aria-label="Pricing model"
			className="flex flex-col gap-3 rounded-md border border-hairline bg-surface p-4"
		>
			<div className="flex flex-wrap items-baseline gap-2">
				<span className="text-2xs text-ink-muted uppercase">Priced under</span>
				<span className="font-medium text-ink">
					{MODEL_LABELS[choice.model]}
				</span>
				<Badge tone={auto ? "accent" : "neutral"}>
					{auto ? "chosen automatically" : "chosen by hand"}
				</Badge>
			</div>
			{auto && <p className="text-sm text-ink-secondary">{choice.reason}</p>}
			{cal && (
				<details className="text-2xs" open={Boolean(cal.heston)}>
					<summary className="cursor-pointer text-ink-muted">
						Calibrated on the {cal.ticker} option chain of {cal.snapshot}
						{cal.heston &&
							` — ${formatDuration(cal.seconds * 1000)}, once per snapshot`}
					</summary>
					{cal.heston ? (
						<dl className="mt-2 grid grid-cols-[max-content_1fr] gap-x-4 gap-y-1">
							<HestonRows h={cal.heston} />
							{cal.leverage && <LeverageRows l={cal.leverage} />}
						</dl>
					) : (
						<p className="mt-2 text-ink-secondary">
							SVI slices per maturity, then the Dupire local-vol grid.
						</p>
					)}
				</details>
			)}
		</section>
	);
}
