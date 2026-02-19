export type Point2D = { x: number; y: number };

export const interpolateAt = (points: Point2D[], x: number) => {
	if (points.length === 0) return 0;
	if (x <= points[0].x) return points[0].y;
	const last = points[points.length - 1];
	if (x >= last.x) return last.y;
	for (let i = 0; i < points.length - 1; i += 1) {
		const p0 = points[i];
		const p1 = points[i + 1];
		if (x >= p0.x && x <= p1.x) {
			const t = (x - p0.x) / (p1.x - p0.x);
			return p0.y + t * (p1.y - p0.y);
		}
	}
	return last.y;
};
