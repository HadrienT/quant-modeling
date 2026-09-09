// Set the theme attribute before first paint to avoid a flash of the wrong
// theme (FOUC). Render-blocking on purpose. Kept as an external file rather than
// an inline <script> so the production CSP can forbid inline scripts entirely
// (blueprint WP 14 §3). src/app/theme.ts takes over once React mounts.
(function () {
	try {
		var stored = localStorage.getItem("qm-theme");
		if (stored === "dark" || stored === "light") {
			document.documentElement.setAttribute("data-theme", stored);
		}
	} catch (e) {
		/* private mode — fall back to prefers-color-scheme */
	}
})();
