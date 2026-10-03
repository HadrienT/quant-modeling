import type { XvaResponse } from "@/shared/api";
import { Badge } from "@/shared/ui";
import { ProfileChart } from "@/shared/viz";
import { Explained, Figure } from "./Explained";
import { amount, pct, signed } from "./format";
import { GREGORY } from "./texts";

const METHOD: Record<string, string> = {
	sa_ccr: "Standardised approach (SA-CCR)",
	internal_model: "Internal models method",
};
const MARGIN: Record<string, string> = {
	simm: "ISDA SIMM",
	regression: "Regression model",
};

/**
 * Initial margin and regulatory capital: what is posted and what is tied up,
 * projected over the life of the trades, with the method the server chose
 * for each and why it chose it.
 */
export function MarginCapitalPanel({ data }: { data: XvaResponse }) {
	const im = data.initial_margin;
	const k = data.capital;
	const open = data.capital_uncollateralised;

	return (
		<div className="flex flex-col gap-4">
			{im ? (
				<section className="flex flex-col gap-3">
					<div className="flex flex-wrap items-center gap-2">
						<h3 className="text-sm font-semibold text-ink">Initial margin</h3>
						<Badge tone="accent">{MARGIN[im.model] ?? im.model}</Badge>
					</div>
					<p className="text-sm text-ink-secondary">{im.reason}</p>
					<div className="grid grid-cols-2 gap-2 sm:grid-cols-4">
						<Figure label="Margin today" value={amount(im.today)} />
						<Figure label="MVA" value={signed(data.adjustments.mva)} />
						{im.simm_today && (
							<>
								<Figure
									label="SIMM delta"
									value={amount(im.simm_today.delta)}
								/>
								<Figure
									label="SIMM vega + curvature"
									value={amount(im.simm_today.vega + im.simm_today.curvature)}
								/>
							</>
						)}
					</div>
					<ProfileChart
						title="Expected initial margin"
						times={im.times}
						lines={[{ label: "E[IM(t)]", values: im.expected }]}
						height={240}
					/>
				</section>
			) : (
				<p className="rounded-sm border border-hairline bg-surface p-3 text-sm text-ink-secondary">
					No initial margin: choose &ldquo;+ initial margin&rdquo; under
					Collateral to see it projected and its funding cost, the MVA.
				</p>
			)}

			<section className="flex flex-col gap-3">
				<div className="flex flex-wrap items-center gap-2">
					<h3 className="text-sm font-semibold text-ink">Regulatory capital</h3>
					<Badge tone="accent">{METHOD[k.method] ?? k.method}</Badge>
				</div>
				<p className="text-sm text-ink-secondary">{k.method_reason}</p>
				<div className="grid grid-cols-2 gap-2 sm:grid-cols-4">
					<Figure
						label="Exposure at default"
						value={amount(k.ead_today)}
						note={
							open ? `${amount(open.ead_today)} without collateral` : undefined
						}
					/>
					<Figure
						label="Default-risk capital"
						value={amount(k.default_capital_today)}
						note={`PD ${pct(k.pd)}, LGD ${pct(k.lgd, 0)}`}
					/>
					<Figure label="CVA capital" value={amount(k.cva_capital_today)} />
					<Figure
						label="KVA"
						value={signed(data.adjustments.kva)}
						note={`at ${pct(k.cost_of_capital, 0)} a year`}
					/>
				</div>
				<p className="text-xs text-ink-muted">{k.pd_reason}</p>
				<ProfileChart
					title="Expected exposure at default"
					times={k.times}
					lines={[
						{ label: "E[EAD(t)]", values: k.expected_ead },
						...(open && open.expected_ead.length === k.expected_ead.length
							? [
									{
										label: "Without collateral",
										values: open.expected_ead,
										dashed: true,
									},
								]
							: []),
					]}
					height={240}
				/>
			</section>
			<Explained title="Why capital has a cost" source={GREGORY}>
				A bank must hold capital against the default of its counterparty and
				against losses on the CVA itself. That capital is tied up for the life
				of the trades and shareholders expect a return on it: the KVA is the
				present value of that return. It is the largest adjustment here on an
				uncollateralised swap, and the one collateral reduces most.
			</Explained>
		</div>
	);
}
