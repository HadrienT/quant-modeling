/**
 * CSS → JS token bridge — blueprint WP 01 §8.
 *
 * Shaders and canvas renderers (three.js, lightweight-charts) cannot read CSS
 * custom properties. This resolves the computed values off <html> and notifies
 * subscribers when the theme changes (data-theme attribute OR the OS setting).
 */

export type ThemeTokens = {
	canvas: string;
	surface: string;
	surfaceRaised: string;
	ink: string;
	inkSecondary: string;
	inkMuted: string;
	hairline: string;
	axis: string;
	accent: string;
	/** Fixed-order categorical palette, slots 1..8 (never cycled). */
	series: string[];
	/** Single-hue magnitude ramp, light → dark. */
	sequential: string[];
	/** Divergent scale: [low, mid(neutral grey), high]. */
	divergent: [string, string];
	diverneutral: string;
	status: {
		good: string;
		warning: string;
		serious: string;
		critical: string;
	};
	pnl: { up: string; down: string };
};

function readVar(style: CSSStyleDeclaration, name: string): string {
	return style.getPropertyValue(name).trim();
}

export function readThemeTokens(): ThemeTokens {
	const s = getComputedStyle(document.documentElement);
	return {
		canvas: readVar(s, "--color-canvas"),
		surface: readVar(s, "--color-surface"),
		surfaceRaised: readVar(s, "--color-surface-raised"),
		ink: readVar(s, "--color-ink"),
		inkSecondary: readVar(s, "--color-ink-secondary"),
		inkMuted: readVar(s, "--color-ink-muted"),
		hairline: readVar(s, "--color-hairline"),
		axis: readVar(s, "--color-axis"),
		accent: readVar(s, "--color-accent"),
		series: [1, 2, 3, 4, 5, 6, 7, 8].map((i) =>
			readVar(s, `--color-series-${i}`),
		),
		sequential: [0, 1, 2, 3, 4].map((i) => readVar(s, `--color-seq-${i}`)),
		divergent: [
			readVar(s, "--color-diverge-low"),
			readVar(s, "--color-diverge-high"),
		],
		diverneutral: readVar(s, "--color-diverge-mid"),
		status: {
			good: readVar(s, "--color-good"),
			warning: readVar(s, "--color-warning"),
			serious: readVar(s, "--color-serious"),
			critical: readVar(s, "--color-critical"),
		},
		pnl: {
			up: readVar(s, "--color-pnl-up"),
			down: readVar(s, "--color-pnl-down"),
		},
	};
}

type Listener = (tokens: ThemeTokens) => void;

/**
 * Subscribe to theme-token changes. Fires on data-theme mutations and on OS
 * colour-scheme changes. Returns an unsubscribe function.
 */
export function onThemeTokensChange(listener: Listener): () => void {
	const emit = () => listener(readThemeTokens());

	const observer = new MutationObserver((records) => {
		if (records.some((r) => r.attributeName === "data-theme")) emit();
	});
	observer.observe(document.documentElement, {
		attributes: true,
		attributeFilter: ["data-theme"],
	});

	const media = window.matchMedia("(prefers-color-scheme: dark)");
	media.addEventListener("change", emit);

	return () => {
		observer.disconnect();
		media.removeEventListener("change", emit);
	};
}

/** Sample a divergent blue↔red scale at t ∈ [-1, 1], grey at 0 (WP 01 §3). */
export function divergentColor(
	t: number,
	tokens: ThemeTokens = readThemeTokens(),
): string {
	const clamped = Math.max(-1, Math.min(1, t));
	const [low, high] = tokens.divergent;
	const mid = tokens.diverneutral;
	return clamped < 0 ? mixHex(mid, low, -clamped) : mixHex(mid, high, clamped);
}

/** Sample the single-hue sequential ramp at t ∈ [0, 1]. */
export function sequentialColor(
	t: number,
	tokens: ThemeTokens = readThemeTokens(),
): string {
	const ramp = tokens.sequential;
	const clamped = Math.max(0, Math.min(1, t));
	const scaled = clamped * (ramp.length - 1);
	const i = Math.min(ramp.length - 2, Math.floor(scaled));
	return mixHex(ramp[i] ?? "#000000", ramp[i + 1] ?? "#000000", scaled - i);
}

function mixHex(a: string, b: string, t: number): string {
	const pa = parseHex(a);
	const pb = parseHex(b);
	const ch = (i: number) => Math.round(pa[i]! + (pb[i]! - pa[i]!) * t);
	return `rgb(${ch(0)} ${ch(1)} ${ch(2)})`;
}

function parseHex(hex: string): [number, number, number] {
	const clean = hex.replace("#", "").trim();
	if (clean.length === 3) {
		return [
			parseInt(clean[0]! + clean[0]!, 16),
			parseInt(clean[1]! + clean[1]!, 16),
			parseInt(clean[2]! + clean[2]!, 16),
		];
	}
	return [
		parseInt(clean.slice(0, 2), 16),
		parseInt(clean.slice(2, 4), 16),
		parseInt(clean.slice(4, 6), 16),
	];
}
