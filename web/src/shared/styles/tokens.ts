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

/**
 * Dark-theme defaults, mirroring theme.css. Used when getComputedStyle can't
 * resolve a custom property yet — the stylesheet not being applied at the moment
 * a chart or the WebGL bridge first reads a token (early module init, jsdom,
 * SSR-style first paint). Without this a chart draws with stroke="" (invisible).
 * Keep in sync with @theme in shared/styles/theme.css.
 */
const DEFAULTS: Record<string, string> = {
	"--color-canvas": "#0b0e13",
	"--color-surface": "#141922",
	"--color-surface-raised": "#1b212c",
	"--color-ink": "#f2f5f8",
	"--color-ink-secondary": "#a9b4c2",
	"--color-ink-muted": "#7d8899",
	"--color-hairline": "#212936",
	"--color-axis": "#38414f",
	"--color-accent": "#3987e5",
	"--color-series-1": "#3987e5",
	"--color-series-2": "#d95926",
	"--color-series-3": "#199e70",
	"--color-series-4": "#c98500",
	"--color-series-5": "#d55181",
	"--color-series-6": "#008300",
	"--color-series-7": "#9085e9",
	"--color-series-8": "#e66767",
	"--color-seq-0": "#cde2fb",
	"--color-seq-1": "#7fb2ee",
	"--color-seq-2": "#3987e5",
	"--color-seq-3": "#1f5bb0",
	"--color-seq-4": "#0d366b",
	"--color-diverge-low": "#2a78d6",
	"--color-diverge-mid": "#7d8899",
	"--color-diverge-high": "#d03b3b",
	"--color-good": "#0ca30c",
	"--color-warning": "#fab219",
	"--color-serious": "#ec835a",
	"--color-critical": "#d03b3b",
	"--color-pnl-up": "#0ca30c",
	"--color-pnl-down": "#d03b3b",
};

function readVar(style: CSSStyleDeclaration, name: string): string {
	return style.getPropertyValue(name).trim() || DEFAULTS[name] || "#000000";
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
