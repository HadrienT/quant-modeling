import type { Meta, StoryObj } from "@storybook/react";
import {
	DeltaBadge,
	EngineTag,
	Freshness,
	Metric,
	MetricRow,
	NumberCell,
	Provenance,
	Uncertainty,
} from "./density";

const meta: Meta = {
	title: "Design system/Numeric density",
};
export default meta;

type Story = StoryObj;

export const Primitives: Story = {
	render: () => (
		<div className="flex flex-col gap-6 text-ink">
			<section className="flex flex-col gap-2">
				<h3 className="text-sm text-ink-secondary">NumberCell</h3>
				<div className="flex gap-6">
					<NumberCell value={128.4213} magnitude="price" />
					<NumberCell value={0.2231} magnitude="vol" />
					<NumberCell value={-0.00004123} magnitude="greek" />
					<NumberCell value={4_250_000} magnitude="notional" />
				</div>
			</section>

			<section className="flex flex-col gap-2">
				<h3 className="text-sm text-ink-secondary">Uncertainty</h3>
				<Uncertainty value={12.3456} stdError={0.0021} magnitude="price" />
				<Uncertainty value={12.3456} stdError={1.1} magnitude="price" />
			</section>

			<section className="flex flex-col gap-2">
				<h3 className="text-sm text-ink-secondary">DeltaBadge</h3>
				<div className="flex gap-6">
					<DeltaBadge value={3.2} />
					<DeltaBadge value={-1.75} />
					<DeltaBadge value={0} />
				</div>
			</section>

			<section className="flex flex-col gap-2">
				<h3 className="text-sm text-ink-secondary">
					Freshness · Provenance · EngineTag
				</h3>
				<div className="flex items-center gap-3">
					<Freshness at={Date.now() - 4 * 60_000} />
					<Freshness at={Date.now() - 40 * 60_000} />
					<Provenance source="market" />
					<Provenance source="manual" />
					<EngineTag engine="mc" />
					<EngineTag engine="analytic" />
				</div>
			</section>

			<MetricRow>
				<Metric
					label="NPV"
					value={<NumberCell value={128.42} magnitude="price" />}
					unit="USD"
					change={<DeltaBadge value={2.14} />}
				/>
				<Metric
					label="MC price"
					value={<Uncertainty value={128.4} stdError={0.03} />}
					footnote="128 000 paths · Sobol"
				/>
				<Metric
					label="Delta"
					value={<NumberCell value={0.5423} magnitude="greek" />}
				/>
				<Metric
					label="Vega"
					value={<NumberCell value={0.1187} magnitude="greek" />}
				/>
				<Metric
					label="Theta"
					value={<NumberCell value={-0.0342} magnitude="greek" />}
				/>
			</MetricRow>
		</div>
	),
};
