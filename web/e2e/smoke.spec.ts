import { expect, test } from "@playwright/test";

// Placeholder journey — real journeys are added by WP 12.
test("app shell renders", async ({ page }) => {
	await page.goto("/");
	await expect(page).toHaveTitle(/Quant Modeling/i);
});
