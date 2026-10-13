import { QueryClientProvider } from "@tanstack/react-query";
import { render, screen, within } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { describe, expect, it, vi } from "vitest";
import {
	type BacktestStrategy,
	type BacktestUniverse,
	makeQueryClient,
} from "@/shared/api";
import fixtures from "@/shared/test/backtest.fixtures.json";
import { TooltipProvider } from "@/shared/ui";
import { BacktestControls } from "./BacktestControls";
import { CostPanel } from "./CostPanel";
import { DataPanel } from "./DataPanel";
import { InferencePanel } from "./InferencePanel";
import { MetricsTable } from "./MetricsTable";
import { ValidationPanel } from "./ValidationPanel";
import {
	DEFAULT_VIEW,
	decodeView,
	encodeView,
	problems,
	strategyRequest,
	validationRequest,
} from "./state";

/**
 * The backtest page against answers recorded from the real API
 * (shared/test/backtest.fixtures.json). The acceptance criterion of WP 24 is
 * checked on what is rendered: no figure without its interval and its
 * convention, an absent metric shown as absent, net next to gross.
 */

const universe = fixtures.universe as BacktestUniverse;
const strategy = fixtures.strategy as unknown as BacktestStrategy;

function wrap(ui: React.ReactNode) {
	return render(
		<QueryClientProvider client={makeQueryClient()}>
			<TooltipProvider delayDuration={0}>{ui}</TooltipProvider>
		</QueryClientProvider>,
	);
}

describe("state", () => {
	it("round-trips a view through the URL and falls back on garbage", () => {
		const view = { ...DEFAULT_VIEW, tickers: ["SPY", "TLT"], costBps: 12 };
		expect(decodeView(encodeView(view))).toEqual(view);
		expect(decodeView("not base64 json")).toEqual(DEFAULT_VIEW);
		expect(decodeView(undefined)).toEqual(DEFAULT_VIEW);
	});

	it("sends every parameter the engine has no default for", () => {
		const request = strategyRequest(DEFAULT_VIEW, "2026-10-09");
		expect(request.end).toBe("2026-10-09");
		expect(request.rebalance).toBe("monthly");
		expect(request.max_weight).toBeCloseTo(0.4);
		for (const key of [
			"proportional_cost_bps",
			"lookback",
			"bootstrap_replications",
			"mean_block_length",
			"seed",
			"newey_west_lags",
		] as const)
			expect(request[key]).toBeTypeOf("number");
	});

	it("puts no bound on a method that takes none", () => {
		const grid = validationRequest(DEFAULT_VIEW, "2026-10-09").grid;
		expect(grid).toHaveLength(9);
		for (const c of grid)
			expect(c.max_weight).toBe(c.method === "minimum_variance" ? 0.4 : 1);
		expect(grid.some((c) => c.method === "maximum_sharpe")).toBe(false);
	});

	it("refuses a cap that cannot add up to 100%", () => {
		const view = { ...DEFAULT_VIEW, tickers: ["SPY", "TLT"], maxWeightPct: 40 };
		expect(problems(view, "2026-10-09")).toHaveLength(1);
		expect(
			problems({ ...view, method: "risk_parity" }, "2026-10-09"),
		).toHaveLength(0);
	});
});

describe("controls", () => {
	it("runs the draft, not every keystroke", async () => {
		const onRun = vi.fn();
		wrap(
			<BacktestControls
				universe={universe}
				view={DEFAULT_VIEW}
				running={false}
				onRun={onRun}
			/>,
		);
		const cost = screen.getByLabelText("Cost of a trade");
		await userEvent.clear(cost);
		await userEvent.type(cost, "25");
		expect(onRun).not.toHaveBeenCalled();
		await userEvent.click(
			screen.getByRole("button", { name: /run backtest/i }),
		);
		expect(onRun).toHaveBeenCalledWith({ ...DEFAULT_VIEW, costBps: 25 });
	});

	it("warns that single stocks are picked with hindsight", () => {
		const stock = universe.stocks[0]!.ticker;
		wrap(
			<BacktestControls
				universe={universe}
				view={{ ...DEFAULT_VIEW, tickers: ["SPY", stock] }}
				running={false}
				onRun={vi.fn()}
			/>,
		);
		expect(screen.getByText(/picked with hindsight/)).toBeInTheDocument();
	});
});

describe("results", () => {
	const columns = [
		{ label: "Net", report: strategy.strategy.report },
		{ label: "Gross", report: strategy.gross!.report },
	];

	it("shows no figure without its interval and its convention", () => {
		wrap(<MetricsTable columns={columns} />);
		const row = screen.getByRole("row", { name: /^Sharpe ratio\b(?!.*boot)/ });
		for (const cell of within(row).getAllByRole("cell")) {
			expect(cell.textContent).toMatch(/\[.+, .+\]/);
			expect(cell.getAttribute("title")).toMatch(/^Convention: .+ Interval: /);
		}
		const cells = screen.getAllByRole("cell");
		expect(cells.length).toBeGreaterThan(20);
		for (const cell of cells)
			if (/\d/.test(cell.textContent ?? ""))
				expect(cell.textContent).toMatch(/\[.+, .+\]/);
	});

	it("shows an omitted metric as absent, with the reason, never as zero", () => {
		const report = {
			...strategy.strategy.report,
			metrics: { ...strategy.strategy.report.metrics },
			omitted: { calmar_ratio: "no drawdown in the sample" },
		};
		delete (report.metrics as Record<string, unknown>).calmar_ratio;
		wrap(<MetricsTable columns={[{ label: "Net", report }]} />);
		const row = screen.getByRole("row", { name: /Calmar ratio/ });
		expect(within(row).getByRole("cell").textContent).toBe("not available");
		expect(
			screen.getByText(/Calmar ratio \(Net\): no drawdown in the sample/),
		).toBeInTheDocument();
	});

	it("puts net next to gross, with turnover and the break-even cost", () => {
		wrap(<CostPanel data={strategy} />);
		expect(screen.getByText("Final value, net of costs")).toBeInTheDocument();
		expect(screen.getByText("Final value, before costs")).toBeInTheDocument();
		expect(screen.getByText("Turnover per year")).toBeInTheDocument();
		expect(screen.getByText(/% interval \[.+ bp, .+ bp\]/)).toBeInTheDocument();
		expect(screen.getByText(/they are an upper bound/)).toBeInTheDocument();
	});

	it("tests the strategy against equal weights and labels the in-sample ratio", () => {
		wrap(<InferencePanel data={strategy} />);
		expect(screen.getAllByText(/p = /).length).toBeGreaterThan(0);
		expect(screen.getByText(/not a forecast/)).toBeInTheDocument();
		expect(screen.getByText(/published with a lag/)).toBeInTheDocument();
	});

	it("displays the engine's conventions word for word, and the run's identity", () => {
		wrap(<DataPanel data={strategy} />);
		for (const text of Object.values(strategy.conventions))
			expect(screen.getByText(text)).toBeInTheDocument();
		expect(
			screen.getByText(new RegExp(strategy.engine.git_sha)),
		).toBeInTheDocument();
		expect(screen.getByText(/186 have a history/)).toBeInTheDocument();
	});

	it("draws the logits and gives the overfitting probability no error bar", async () => {
		wrap(
			<ValidationPanel
				request={validationRequest(DEFAULT_VIEW, "2024-12-31")}
			/>,
		);
		expect(
			await screen.findByText(/it has no standard error/),
		).toBeInTheDocument();
		expect(screen.getByText(/the logits/)).toBeInTheDocument();
		expect(screen.getByText(/is not counted here/)).toBeInTheDocument();
		expect(screen.getAllByText(/^#\d+ /).length).toBeGreaterThan(0);
	});
});
