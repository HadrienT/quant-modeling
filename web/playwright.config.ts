import { defineConfig, devices } from "@playwright/test";

/**
 * Five e2e journeys only (WP 12 §1), run against the real docker compose stack.
 * BASE_URL defaults to the dev server.
 */
export default defineConfig({
	testDir: "./e2e",
	fullyParallel: true,
	forbidOnly: !!process.env.CI,
	retries: process.env.CI ? 2 : 0,
	reporter: process.env.CI ? "github" : "list",
	use: {
		baseURL: process.env.QM_E2E_BASE_URL ?? "http://localhost:5173",
		trace: "on-first-retry",
	},
	projects: [{ name: "chromium", use: { ...devices["Desktop Chrome"] } }],
});
