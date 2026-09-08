import { StrictMode } from "react";
import { createRoot } from "react-dom/client";
import { BrowserRouter } from "react-router-dom";
import "@/shared/styles/theme.css";
import "katex/dist/katex.min.css";
import "./styles.css";
import App from "./App";

const root = document.getElementById("root");
if (!root) throw new Error("#root not found");

createRoot(root).render(
	<StrictMode>
		<BrowserRouter>
			<App />
		</BrowserRouter>
	</StrictMode>,
);
