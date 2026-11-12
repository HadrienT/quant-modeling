import type { ReactNode } from "react";
import type { XvaResponse } from "@/shared/api";
import { Tabs, TabsContent, TabsList, TabsTrigger } from "@/shared/ui";
import { AdjustmentsPanel } from "./AdjustmentsPanel";
import { ExposurePanel } from "./ExposurePanel";
import { MarginCapitalPanel } from "./MarginCapitalPanel";
import { TradesTable } from "./TradesTable";

export type XvaResultTab =
	"exposure" | "adjustments" | "capital" | "sensitivities";

/**
 * The four tabs of an xVA run: exposure, adjustments with each trade's
 * share, margin and capital, and the sensitivities, which each page fetches
 * its own way. Uncontrolled unless `value` is given (the xVA page keeps the
 * tab in the URL).
 */
export function XvaResultTabs({
	data,
	sensitivities,
	value,
	onValueChange,
}: {
	data: XvaResponse;
	sensitivities: ReactNode;
	value?: XvaResultTab;
	onValueChange?: (tab: XvaResultTab) => void;
}) {
	const control =
		value === undefined
			? { defaultValue: "exposure" }
			: {
					value,
					onValueChange: (v: string) => onValueChange?.(v as XvaResultTab),
				};
	return (
		<Tabs {...control}>
			<TabsList>
				<TabsTrigger value="exposure">Exposure</TabsTrigger>
				<TabsTrigger value="adjustments">Adjustments</TabsTrigger>
				<TabsTrigger value="capital">Margin and capital</TabsTrigger>
				<TabsTrigger value="sensitivities">Sensitivities</TabsTrigger>
			</TabsList>
			<TabsContent value="exposure">
				<ExposurePanel data={data} />
			</TabsContent>
			<TabsContent value="adjustments">
				<div className="flex flex-col gap-6">
					<AdjustmentsPanel data={data} />
					<TradesTable data={data} />
				</div>
			</TabsContent>
			<TabsContent value="capital">
				<MarginCapitalPanel data={data} />
			</TabsContent>
			<TabsContent value="sensitivities">{sensitivities}</TabsContent>
		</Tabs>
	);
}
