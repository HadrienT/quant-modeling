import { StrictMode } from "react";
import { createRoot } from "react-dom/client";
import { loadConfig } from "@/shared/config";
import "@/shared/styles/theme.css";
// KaTeX CSS is loaded lazily by shared/products/Formula when a doc card opens.
import { App } from "@/app/App";

const root = document.getElementById("root");
if (!root) throw new Error("#root not found");

// Resolve runtime config (API base URL) before the first request.
loadConfig().finally(() => {
	createRoot(root).render(
		<StrictMode>
			<App />
		</StrictMode>,
	);
});
