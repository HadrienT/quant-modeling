import { expect, test } from "@playwright/test";

/**
 * The five e2e journeys — blueprint WP 12 §1. Run against the real
 * `docker compose` stack (QM_E2E_BASE_URL). More than five costs without gain.
 */

test("price a vanilla option and see its standard error", async ({ page }) => {
	await page.goto("/price?product=vanilla&engine=mc");
	await expect(
		page.getByRole("heading", { name: /european option/i }),
	).toBeVisible();
	// a Monte-Carlo price must show "± <std error>"
	await expect(page.getByText(/±/)).toBeVisible({ timeout: 30_000 });
});

test("display a volatility surface", async ({ page }) => {
	await page.goto("/market?tab=vol&ticker=AAPL");
	await expect(page.getByText(/raw quotes → cleaning/i)).toBeVisible();
	// either the WebGL canvas or the 2D fallback
	await expect(page.locator("canvas, table")).toBeVisible({ timeout: 30_000 });
});

test("create a portfolio and value it", async ({ page }) => {
	await page.goto("/portfolio");
	await page.getByRole("button", { name: /^new$/i }).click();
	await page.getByRole("button", { name: /add position/i }).click();
	await page.getByRole("button", { name: /add to portfolio/i }).click();
	await expect(page.getByText(/Total \(1\)/)).toBeVisible();
});

test("run a backtest", async ({ page }) => {
	await page.goto("/backtest");
	await page.getByRole("button", { name: /run backtest/i }).click();
	await expect(page.getByText(/equity curve/i)).toBeVisible({
		timeout: 60_000,
	});
});

test("sign in and out", async ({ page }) => {
	await page.goto("/");
	await page.getByRole("button", { name: /sign in/i }).click();
	const suffix = Date.now();
	await page.getByLabel("Username").fill(`e2e${suffix}`);
	await page.getByLabel("Password").fill("password123");
	await page.getByRole("button", { name: /register/i }).click();
	await expect(page.getByRole("button", { name: /log out/i })).toBeVisible();
	await page.getByRole("button", { name: /log out/i }).click();
	await expect(page.getByRole("button", { name: /sign in/i })).toBeVisible();
});
