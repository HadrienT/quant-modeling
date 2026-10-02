import type { SwaptionPricingResponse } from "@/shared/api";
import { Badge } from "@/shared/ui";

type Swaption = SwaptionPricingResponse["swaption"];

const LABELS: Record<Swaption["model"], string> = {
	bachelier: "Bachelier (normal)",
	black: "Black (shifted lognormal)",
	sabr: "SABR (shifted)",
	hull_white: "Hull-White (calibrated)",
};

/** Which model the swaption is valued under, whether it was picked for the
 * user, and why: the reason is the API's, next to the code that chooses. */
export function RatesModelDecision({ swaption }: { swaption: Swaption }) {
	const auto = swaption.requested === "auto";
	return (
		<section
			aria-label="Pricing model"
			className="flex flex-col gap-2 rounded-md border border-hairline bg-surface p-4"
		>
			<div className="flex flex-wrap items-baseline gap-2">
				<span className="text-2xs text-ink-muted uppercase">Priced under</span>
				<span className="font-medium text-ink">{LABELS[swaption.model]}</span>
				<Badge tone={auto ? "accent" : "neutral"}>
					{auto ? "chosen automatically" : "chosen by hand"}
				</Badge>
			</div>
			{auto && <p className="text-sm text-ink-secondary">{swaption.reason}</p>}
		</section>
	);
}
