export {
	type SurfaceGrid,
	type AxisSpec,
	makeGrid,
	zAt,
	zExtent,
	holeFraction,
} from "./SurfaceGrid";
export { buildSurfaceGeometry, nearestNode } from "./geometry";
export {
	ivSurfaceToGrid,
	cleanedIvSurfaceToGrid,
	localVolSurfaceToGrid,
	differenceGrid,
} from "./adapters";
export { SurfaceView } from "./SurfaceView";
