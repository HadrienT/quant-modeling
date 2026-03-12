import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

export default defineConfig({
	plugins: [react()],
	define: {
		__COMMIT_SHA__: JSON.stringify(process.env.VITE_COMMIT_SHA ?? "dev"),
	},
	server: {
		port: 5173,
		host: true,
		proxy: {
			"/api": {
				target: process.env.VITE_API_PROXY_TARGET ?? "http://localhost:8000",
				changeOrigin: true,
				rewrite: (path) => path.replace(/^\/api/, "")
			}
		}
	}
});
