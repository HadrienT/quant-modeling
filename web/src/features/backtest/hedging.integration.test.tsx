import { QueryClientProvider } from "@tanstack/react-query";
import { render, screen, within } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { HttpResponse, http } from "msw";
import { describe, expect, it } from "vitest";
import { type BacktestTask, makeQueryClient } from "@/shared/api";
import fixture from "@/shared/test/hedging.fixture.json";
import { server } from "@/shared/test/msw-server";
import { HedgingSection } from "./HedgingSection";
import { DEFAULT_FORM, hedgingRequest, withInterval } from "./hedging";

/**
 * The hedging tab against an answer recorded from the real API
 * (shared/test/hedging.fixture.json, a Black-Scholes world): the task is
 * started, followed, and its result shown, every mean and standard
 * deviation with its interval.
 */

const done = fixture as unknown as BacktestTask;

function wrap(ui: React.ReactNode) {
	return render(
		<QueryClientProvider client={makeQueryClient()}>{ui}</QueryClientProvider>,
	);
}

describe("hedging request", () => {
	it("sends fractions, the world's model and its ticker only when calibrated", () => {
		const r = hedgingRequest(DEFAULT_FORM);
		expect(r.world!.model).toBe("heston");
		expect(r.world!.ticker).toBeNull();
		expect(r.strike).toBeCloseTo(1);
		expect(r.world!.rate).toBeCloseTo(0.04);
		const onTicker = hedgingRequest({
			...DEFAULT_FORM,
			world: "ticker",
			ticker: " spy ",
		});
		expect(onTicker.world).toMatchObject({ model: "heston", ticker: "SPY" });
		const bs = hedgingRequest({
			...DEFAULT_FORM,
			world: "black_scholes",
			costBps: "5",
		});
		expect(bs.world!.model).toBe("black_scholes");
		expect(bs.cost_rate).toBeCloseTo(0.0005);
	});

	it("writes a metric as its value and its half-width", () => {
		const m = {
			value: -0.5,
			lower: -0.6,
			upper: -0.4,
			standard_error: 0.05,
			confidence: 0.95,
			method: "",
			convention: "",
		};
		expect(withInterval(m)).toBe("−0.500 ± 0.100");
		expect(withInterval(null)).toBe("n/a");
	});
});

describe("hedging section", () => {
	it("starts a task, follows it and shows every hedge with its intervals", async () => {
		const bodies: unknown[] = [];
		server.use(
			http.post("*/api/backtest/hedging", async ({ request }) => {
				bodies.push(await request.json());
				return HttpResponse.json(
					{ ...done, status: "queued", progress: 0, hedging: null },
					{ status: 202 },
				);
			}),
			http.get("*/api/backtest/tasks/t1", () => HttpResponse.json(done)),
		);
		wrap(<HedgingSection />);
		await userEvent.click(
			screen.getByRole("button", { name: /run the hedging backtest/i }),
		);
		await screen.findByRole("columnheader", { name: "Derman-Kamal floor" });
		const table = screen.getAllByRole("table")[0]!; // the statistics, first
		expect(bodies).toHaveLength(1);
		for (const h of done.hedging!.hedges) {
			const row = within(table)
				.getAllByRole("row")
				.find((r) => r.textContent?.includes(withInterval(h.mean)));
			expect(row, h.hedge).toBeTruthy();
			expect(row!.textContent).toContain(withInterval(h.standard_deviation));
		}
		expect(screen.getByText(/fitted on another seed/)).toBeInTheDocument();
	});
});
