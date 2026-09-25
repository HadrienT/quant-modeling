import { useCallback, useEffect, useRef, useState } from "react";

const ANIMATION_MS = 2200;

/** The client-side "drawing" of an already-fetched batch of paths: how many
 * time steps are revealed so far, and a way to (re)play it. */
export function usePathReveal() {
	const [revealCount, setRevealCount] = useState(0);
	const animRef = useRef<number | undefined>(undefined);

	const playAnimation = useCallback((steps: number) => {
		if (animRef.current !== undefined) cancelAnimationFrame(animRef.current);
		const start = performance.now();
		const step = (now: number) => {
			const frac = Math.min((now - start) / ANIMATION_MS, 1);
			setRevealCount(Math.round(frac * steps));
			if (frac < 1) animRef.current = requestAnimationFrame(step);
		};
		animRef.current = requestAnimationFrame(step);
	}, []);

	useEffect(() => {
		return () => {
			if (animRef.current !== undefined) cancelAnimationFrame(animRef.current);
		};
	}, []);

	return { revealCount, setRevealCount, playAnimation };
}
