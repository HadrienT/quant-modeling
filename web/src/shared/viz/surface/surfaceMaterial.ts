import * as THREE from "three";
import { sequentialColor } from "@/shared/styles/tokens";
import type { ThemeTokens } from "@/shared/styles/tokens";

/**
 * Single-hue colormap in a 256px LUT, isolines in the fragment shader
 * (blueprint WP 05 §3). No turbo / jet / rainbow — on a lit surface the shading
 * already modulates lightness; a colormap that also modulates it makes both the
 * shape and the value unreadable. Height carries the magnitude; colour is
 * redundant and that is fine. Precision is read from the isolines.
 */

export function buildLut(
	tokens: ThemeTokens,
	mode: "sequential" | "divergent",
): THREE.DataTexture {
	const size = 256;
	const data = new Uint8Array(size * 4);
	for (let i = 0; i < size; i++) {
		const t = i / (size - 1);
		let rgb: string;
		if (mode === "divergent") {
			// -1..1 mapped from 0..1
			const c = t * 2 - 1;
			const [low, high] = tokens.divergent;
			const mid = tokens.diverneutral;
			rgb = c < 0 ? mix(mid, low, -c) : mix(mid, high, c);
		} else {
			rgb = sequentialColor(t, tokens);
		}
		const [r, g, b] = parseRgb(rgb);
		data[i * 4] = r;
		data[i * 4 + 1] = g;
		data[i * 4 + 2] = b;
		data[i * 4 + 3] = 255;
	}
	const tex = new THREE.DataTexture(data, size, 1, THREE.RGBAFormat);
	tex.needsUpdate = true;
	tex.minFilter = THREE.LinearFilter;
	tex.magFilter = THREE.LinearFilter;
	return tex;
}

function parseRgb(s: string): [number, number, number] {
	const m = s.match(/-?\d+(\.\d+)?/g)!.map(Number);
	if (s.startsWith("#")) {
		const h = s.slice(1);
		return [
			parseInt(h.slice(0, 2), 16),
			parseInt(h.slice(2, 4), 16),
			parseInt(h.slice(4, 6), 16),
		];
	}
	return [m[0] ?? 0, m[1] ?? 0, m[2] ?? 0];
}

function mix(a: string, b: string, t: number): string {
	const pa = parseRgb(a);
	const pb = parseRgb(b);
	return `rgb(${pa.map((c, i) => Math.round(c + (pb[i]! - c) * t)).join(",")})`;
}

export const surfaceVertexShader = /* glsl */ `
	attribute float aValue;
	varying float vValue;
	varying vec3 vNormalW;
	varying vec3 vWorld;
	void main() {
		vValue = aValue;               // 0..1 normalised height, -1 = hole
		vNormalW = normalize(normalMatrix * normal);
		vec4 world = modelMatrix * vec4(position, 1.0);
		vWorld = world.xyz;
		gl_Position = projectionMatrix * viewMatrix * world;
	}
`;

export const surfaceFragmentShader = /* glsl */ `
	precision highp float;
	uniform sampler2D uLut;
	uniform vec3 uLightDir;
	uniform vec3 uAmbient;
	uniform float uIsoStep;       // isoline spacing in normalised value
	uniform vec3 uIsoColor;
	uniform float uShowGridLines;
	varying float vValue;
	varying vec3 vNormalW;
	varying vec3 vWorld;

	void main() {
		if (vValue < -0.5) discard;   // hole: no material

		vec3 base = texture2D(uLut, vec2(clamp(vValue, 0.0, 1.0), 0.5)).rgb;

		vec3 n = normalize(vNormalW);
		float key = max(dot(n, normalize(uLightDir)), 0.0);
		float fill = max(dot(n, normalize(vec3(-uLightDir.x, 0.4, -uLightDir.z))), 0.0) * 0.35;
		vec3 lit = base * (uAmbient + key + fill);

		// anti-aliased isolines on the interpolated value
		float f = vValue / uIsoStep;
		float d = abs(fract(f - 0.5) - 0.5) / fwidth(f);
		float iso = 1.0 - clamp(d - 0.5, 0.0, 1.0);
		lit = mix(lit, uIsoColor, iso * 0.55);

		gl_FragColor = vec4(pow(lit, vec3(1.0 / 2.2)), 1.0);
	}
`;
