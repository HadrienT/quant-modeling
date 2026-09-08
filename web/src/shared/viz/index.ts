export { ChartFrame, type LegendItem, type TableView } from "./ChartFrame";
export { Cartesian } from "./Cartesian";
export { useChartTheme, chartTheme } from "./theme";
export { fmt, niceDomain } from "./format";
export { useMeasure } from "./useMeasure";

export { PayoffChart, type PayoffSeries } from "./charts/PayoffChart";
export {
	GreekProfileChart,
	type GreekProfile,
} from "./charts/GreekProfileChart";
export {
	ConvergenceChart,
	type ConvergencePoint,
} from "./charts/ConvergenceChart";
export { AllocationChart, type AllocationRow } from "./charts/AllocationChart";
export { DistributionChart } from "./charts/DistributionChart";
export { RatesCurveChart, type RateSeries } from "./charts/RatesCurveChart";
export { SmileChart, type SmileSlice } from "./charts/SmileChart";
export { SurfaceHeatmap, type Grid } from "./charts/SurfaceHeatmap";
export { StressMatrix, type StressCell } from "./charts/StressMatrix";
export {
	PriceSeriesChart,
	EquityAndDrawdownChart,
	type Candle,
	type LinePoint,
} from "./charts/lightweight";
export {
	SurfaceView,
	makeGrid,
	zAt,
	zExtent,
	holeFraction,
	ivSurfaceToGrid,
	cleanedIvSurfaceToGrid,
	localVolSurfaceToGrid,
	differenceGrid,
	checkArbitrage,
	coverage,
	type SurfaceGrid,
	type ArbViolation,
} from "./surface";
