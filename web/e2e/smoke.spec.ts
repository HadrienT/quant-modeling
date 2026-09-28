import { expect, test } from "@playwright/test";

// Placeholder journey — real journeys are added by WP 12.
test("app shell renders", async ({ page }) => {
	await page.goto("/");
	await expect(page).toHaveTitle(/Quant Modeling/i);
});

// #78: at phone width no page scrolls sideways; the nav folds into a menu.
test("no horizontal scroll at 390 px", async ({ page }) => {
	await page.setViewportSize({ width: 390, height: 844 });
	for (const path of [
		"/visualize",
		"/market",
		"/price",
		"/simulation",
		"/scripting",
		"/products",
		"/portfolio",
		"/backtest",
		"/about",
	]) {
		await page.goto(path);
		await page.waitForLoadState("networkidle");
		const overflow = await page.evaluate(
			() => document.documentElement.scrollWidth - window.innerWidth,
		);
		expect(overflow, path).toBeLessThanOrEqual(0);
	}
	await page.getByRole("button", { name: /open menu/i }).click();
	await expect(page.getByRole("navigation", { name: "Pages" })).toBeVisible();
});
