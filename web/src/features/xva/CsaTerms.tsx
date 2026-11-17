import { Segmented } from "@/shared/ui";
import { MPORS, MTAS, THRESHOLDS, type XvaView } from "./state";

const amount = (v: number) =>
	v === 0
		? "0"
		: new Intl.NumberFormat("en-US", { notation: "compact" }).format(v);

const FLOWS: { value: XvaView["flows"]; label: string; title: string }[] = [
	{
		value: "withheld",
		label: "Withheld",
		title:
			"Both parties stop every payment with the margin calls: the period's flows stay in the close-out amount, no spike (Andersen, Pykhtin & Sokol 2017, 'Classical+').",
	},
	{
		value: "paid",
		label: "Paid",
		title:
			"Both parties keep paying the trade flows up to the close-out: a flow the bank pays inside the period makes the value jump while the collateral still reflects the value before it ('Classical−').",
	},
	{
		value: "only_bank_pays",
		label: "Only the bank pays",
		title:
			"The adverse case: the bank keeps paying, the counterparty does not — the spikes of 'Paid', plus what the counterparty owed and did not pay.",
	},
];

/**
 * The terms of the collateral agreement (WP 23, P3 and B8): the threshold
 * (the same on both sides), the minimum transfer amount, the margin period of
 * risk, and what the trades' own flows do during it. Under "Paid" the
 * exposure spikes on the bank's payment dates, most visibly under initial
 * margin, which covers the moves of the period but not a payment.
 */
export function CsaTerms({
	view,
	onChange,
}: {
	view: XvaView;
	onChange: (patch: Partial<XvaView>) => void;
}) {
	const row = (label: string, child: React.ReactNode) => (
		<div className="flex items-center gap-2 text-xs text-ink-secondary">
			<span>{label}</span>
			{child}
		</div>
	);
	return (
		<div className="flex flex-col gap-2 rounded-sm border border-hairline/70 p-2">
			<div className="flex flex-wrap items-center gap-x-6 gap-y-2">
				{row(
					"Threshold",
					<Segmented
						label="Threshold, both sides"
						options={THRESHOLDS.map((v) => ({ value: v, label: amount(v) }))}
						value={view.threshold}
						onChange={(threshold) => onChange({ threshold })}
					/>,
				)}
				{row(
					"Minimum transfer",
					<Segmented
						label="Minimum transfer amount"
						options={MTAS.map((v) => ({ value: v, label: amount(v) }))}
						value={view.mta}
						onChange={(mta) => onChange({ mta })}
					/>,
				)}
				{row(
					"Margin period of risk",
					<Segmented
						label="Margin period of risk, business days"
						options={MPORS.map((v) => ({ value: v, label: `${v} days` }))}
						value={view.mpor}
						onChange={(mpor) => onChange({ mpor })}
					/>,
				)}
				{row(
					"Flows during it",
					<Segmented
						label="Trade flows during the margin period of risk"
						options={FLOWS}
						value={view.flows}
						onChange={(flows) => onChange({ flows })}
					/>,
				)}
			</div>
			{view.flows !== "withheld" && (
				<p className="text-2xs text-warning">
					The bank's payments inside the margin period of risk are not
					collateralised: the exposure profile spikes on each of its payment
					dates
					{view.csa === "im" ? ", which initial margin does not cover" : ""}.
					Those spikes are the model, not noise.
				</p>
			)}
		</div>
	);
}
