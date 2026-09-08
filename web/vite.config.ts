/// <reference types="vitest/config" />
import { resolve } from "node:path";
import { defineConfig, type PluginOption } from "vite";
import react from "@vitejs/plugin-react";
import tailwindcss from "@tailwindcss/vite";
import { visualizer } from "rollup-plugin-visualizer";

// VITE_API_KEY is deliberately NOT read here — see blueprint ADR-008.
// The API base URL is resolved at runtime from /config.json (see src/shared/config),
// import.meta.env.VITE_API_BASE is only a dev fallback.

export default defineConfig({
	plugins: [
		react(),
		tailwindcss(),
		visualizer({
			filename: "dist/bundle-report.html",
			gzipSize: true,
			brotliSize: true,
		}) as PluginOption,
	],
	resolve: {
		alias: {
			"@": resolve(__dirname, "src"),
		},
	},
	server: {
		port: 5173,
		host: true,
		proxy: Object.fromEntries(
			// `^/market/` (not `/market`) so the SPA routes /market and /price are
			// served by Vite while the API's /market/* and /price/* are proxied.
			["/api", "^/market/", "^/price/", "/health", "/openapi.json"].map((p) => [
				p,
				{
					target: process.env.VITE_API_PROXY_TARGET ?? "http://localhost:8000",
					changeOrigin: true,
				},
			]),
		),
	},
	build: {
		sourcemap: true,
		rollupOptions: {
			output: {
				manualChunks: {
					"react-vendor": ["react", "react-dom"],
					"router-vendor": ["@tanstack/react-router", "@tanstack/react-query"],
				},
			},
		},
	},
	test: {
		globals: true,
		environment: "jsdom",
		setupFiles: ["./tests/setup.ts"],
		css: false,
		include: [
			"src/**/*.{test,spec}.{ts,tsx}",
			"tests/**/*.{test,spec}.{ts,tsx}",
		],
	},
});
