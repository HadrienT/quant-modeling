import { QueryClientProvider } from "@tanstack/react-query";
import { fireEvent, render, screen, within } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { HttpResponse, http } from "msw";
import { describe, expect, it, vi } from "vitest";
import {
	type XvaPortfolios,
	type XvaResponse,
	makeQueryClient,
} from "@/shared/api";
import { server } from "@/shared/test/msw-server";
import fixtures from "@/shared/test/xva.fixtures.json";
import { TooltipProvider } from "@/shared/ui";
import { AdjustmentsPanel } from "./AdjustmentsPanel";
import { ExposurePanel } from "./ExposurePanel";
import { MarginCapitalPanel } from "./MarginCapitalPanel";
import { SensitivitiesPanel } from "./SensitivitiesPanel";
import { amount } from "./format";
import { DEFAULT_VIEW, nettingSetRequest, sensitivitiesRequest } from "./state";
import { TradesTable } from "./TradesTable";
import { XvaControls } from "./XvaControls";

/**
 * The xVA page against answers recorded from the real API
 * (shared/test/xva.fixtures.json): every figure is shown signed as a cash
 * flow with its Monte-Carlo error, each choice the server made comes with its
 * reason, the wrong-way scenarios explain themselves on hover, and a
 * portfolio the sensitivities do not cover says why.
 */

const options = fixtures.portfolios as XvaPortfolios;
const swap = fixtures.single_swap as unknown as XvaResponse;
const book = fixtures.balanced_csa as unknown as XvaResponse;

function wrap(ui: React.ReactNode) {
	return render(
		<QueryClientProvider client={makeQueryClient()}>
			<TooltipProvider delayDuration={0}>{ui}</TooltipProvider>
		</QueryClientProvider>,
	);
}

describe("controls", () => {
	it("offers the wrong-way scenarios as buttons", () => {
		const onChange = vi.fn();
		wrap(
			<XvaControls options={options} view={DEFAULT_VIEW} onChange={onChange} />,
		);
		const group = screen.getByRole("group", {
			name: "Wrong-way risk scenario",
		});
		const buttons = within(group).getAllByRole("button");
		expect(buttons.map((b) => b.textContent)).toEqual([
			"Independent",
			"Wrong-way",
			"Strong wrong-way",
			"Right-way",
		]);
		expect(buttons[0]).toHaveAttribute("aria-pressed", "true");

		// The explanation of each is a Radix tooltip, which jsdom cannot open
		// (no layout): the text it shows is the API's, checked here; the
		// hover itself is checked in a browser. For the same reason the click
		// is a bare event: a pointer would open the tooltip.
		expect(options.wrong_way_scenarios[1]!.explanation).toMatch(
			/hazard rate rises by 65 %.*A scenario, not an estimate/,
		);

		fireEvent.click(buttons[1]!);
		expect(onChange).toHaveBeenCalledWith({ wwr: 10 });
	});

	it("offers every portfolio but the one that needs scripts of its own", () => {
		wrap(
			<XvaControls options={options} view={DEFAULT_VIEW} onChange={() => {}} />,
		);
		const group = screen.getByRole("group", { name: "Portfolio" });
		const labels = within(group)
			.getAllByRole("button")
			.map((b) => b.textContent);
		expect(labels).toContain("One 10-year payer swap");
		expect(labels).toContain("A swap written as a script");
		expect(labels).not.toContain("Your own scripts");
		// The lesson of each portfolio is its hover text.
		expect(
			within(group).getByRole("button", { name: "One bought swaption" }),
		).toHaveAttribute("title", expect.stringContaining("can only be an asset"));
	});

	it("builds the request the page's view stands for", () => {
		expect(nettingSetRequest(DEFAULT_VIEW)).toMatchObject({
			portfolio: "single_swap",
			counterparty_rating: "BBB",
			csa: null,
			borrowing_spread: 0.005,
			wrong_way_risk: 0,
		});
		const margined = nettingSetRequest({ ...DEFAULT_VIEW, csa: "im", wwr: 10 });
		expect(margined.csa).toMatchObject({
			initial_margin: true,
			margin_period_of_risk_days: 10,
			threshold_counterparty: 0,
		});
		expect(margined.wrong_way_risk).toBe(10);
		// The sensitivities are asked for the same netting set.
		expect(sensitivitiesRequest({ ...DEFAULT_VIEW, csa: "vm" })).toMatchObject({
			portfolio: "single_swap",
			csa: { initial_margin: false },
		});
	});
});

describe("adjustments", () => {
	it("signs every adjustment as a cash flow, with its Monte-Carlo error", () => {
		wrap(<AdjustmentsPanel data={swap} />);
		const cva = screen.getByRole("row", { name: /^CVA/ });
		// A cost: negative, with a true minus sign; the error next to it.
		expect(cva).toHaveTextContent("−24,664");
		expect(cva).toHaveTextContent("± 681");
		expect(screen.getByRole("row", { name: /^DVA/ })).toHaveTextContent(
			"+16,863",
		);
		// Every simulated adjustment carries its error: the funding ones (the
		// error of the sum, smaller than the sum of the two) and the capital.
		const fva = screen.getByRole("row", { name: /^FVA/ });
		expect(fva).toHaveTextContent("−90");
		expect(fva).toHaveTextContent("± 539");
		const kva = screen.getByRole("row", { name: /^KVA/ });
		expect(kva).toHaveTextContent("−76,145");
		expect(kva).toHaveTextContent("± 1,741");
		// No collateral, no margin: nothing simulated, no error to show.
		expect(screen.getByRole("row", { name: /^ColVA/ })).not.toHaveTextContent(
			"±",
		);
		// No collateral: one column, and the convention is stated.
		expect(screen.queryByText("Without collateral")).not.toBeInTheDocument();
		expect(
			screen.getByText(/a cost to the bank is negative/),
		).toBeInTheDocument();
		// Each adjustment is explained, with its source.
		expect(
			screen.getByRole("heading", {
				name: "CVA — Credit valuation adjustment",
			}),
		).toBeInTheDocument();
		expect(
			screen.getAllByText(/Gregory, The xVA Challenge/).length,
		).toBeGreaterThan(3);
	});

	it("puts the same netting set without its collateral next to the CSA", () => {
		wrap(<AdjustmentsPanel data={book} />);
		expect(screen.getByText("Under the CSA")).toBeInTheDocument();
		expect(screen.getByText("Without collateral")).toBeInTheDocument();
		const cells = within(
			screen.getByRole("row", { name: /^CVA/ }),
		).getAllByRole("cell");
		const amount = (text: string | null) =>
			Number((text ?? "").replace(/[^0-9]/g, "").slice(0, 9));
		// The CSA leaves a fraction of the CVA.
		expect(amount(cells[1]!.textContent?.split("±")[0] ?? "")).toBeLessThan(
			amount(cells[2]!.textContent),
		);
	});

	it("gives each trade its three shares of the CVA", () => {
		wrap(<TradesTable data={swap} />);
		const row = screen.getByRole("row", { name: /Payer swap 10Y at par/ });
		// Alone, the three are the same number: the CVA of the set.
		expect(within(row).getAllByText("−24,664").length).toBe(3);
	});
});

describe("margin and capital", () => {
	it("says which method measured the capital, and why", () => {
		wrap(<MarginCapitalPanel data={swap} />);
		expect(
			screen.getByText("Standardised approach (SA-CCR)"),
		).toBeInTheDocument();
		expect(screen.getByText(swap.capital.method_reason)).toBeInTheDocument();
		// The probability of default is the historical rate of the rating.
		expect(
			screen.getByText(/Average one-year default rate of S&P's BBB/),
		).toBeInTheDocument();
		expect(screen.getByText(/No initial margin/)).toBeInTheDocument();
	});

	it("shows the initial margin with its model and its reason", () => {
		wrap(<MarginCapitalPanel data={book} />);
		expect(screen.getByText("ISDA SIMM")).toBeInTheDocument();
		expect(screen.getByText(book.initial_margin!.reason)).toBeInTheDocument();
		expect(screen.getByText("Expected initial margin")).toBeInTheDocument();
		expect(screen.getByText(/without collateral/)).toBeInTheDocument();
	});
});

describe("exposure", () => {
	it("shows the profile under the pricing measure, and the real-world one on demand", async () => {
		wrap(<ExposurePanel data={swap} />);
		expect(screen.getByText("Exposure profile")).toBeInTheDocument();
		const fan = "1–99 %, 5–95 % and 20–80 % of the value";
		expect(screen.getByText(fan)).toBeInTheDocument();
		expect(screen.getByText(amount(swap.exposure.epe))).toBeInTheDocument();
		// EPE, Effective EPE and the peak PFE with their errors.
		expect(screen.getByText("± 9,177")).toBeInTheDocument();
		expect(screen.getByText("± 6,633")).toBeInTheDocument();
		const peak = swap.exposure.pfe.indexOf(Math.max(...swap.exposure.pfe));
		expect(
			screen.getByText(`± ${amount(swap.exposure.pfe_error[peak])}`),
		).toBeInTheDocument();
		// The distribution behind the profiles, scenario by date (jsdom has no
		// WebGL: the surface degrades to its heatmap, the title stays).
		const surface = /Value of the netting set: scenario × time/;
		expect(screen.getAllByText(surface).length).toBeGreaterThan(0);
		await userEvent.click(
			screen.getByRole("button", { name: "Real-world measure" }),
		);
		// Other scenarios, another exposure; the fan is the pricing measure's.
		expect(swap.risk.epe).not.toBe(swap.exposure.epe);
		expect(screen.getByText(amount(swap.risk.epe))).toBeInTheDocument();
		expect(screen.getByText("± 1,910")).toBeInTheDocument();
		expect(screen.queryByText(fan)).not.toBeInTheDocument();
		expect(screen.queryByText(surface)).not.toBeInTheDocument();
	});
});

describe("sensitivities", () => {
	const request = sensitivitiesRequest(DEFAULT_VIEW);

	it("are computed on demand, each with its error, with what they cost", async () => {
		wrap(<SensitivitiesPanel request={request} />);
		// Nothing is asked before the button.
		expect(screen.queryByText("Par swap rates")).not.toBeInTheDocument();
		await userEvent.click(
			screen.getByRole("button", { name: "Compute the sensitivities" }),
		);
		expect(await screen.findByText("Par swap rates")).toBeInTheDocument();
		expect(screen.getByText("8.4 valuations")).toBeInTheDocument();
		expect(
			screen.getByText(`${fixtures.sensitivities.bump_valuations} valuations`),
		).toBeInTheDocument();
		// One row per quote, per basis point, with ±.
		const rates = screen.getByRole("table", { name: /Par swap rates/ });
		const ten = within(rates).getByRole("row", { name: /^10Y/ });
		expect(ten).toHaveTextContent(/−\d/);
		expect(ten).toHaveTextContent("±");
		// SA-CVA from these sensitivities, and how they are computed.
		expect(screen.getByText("SA-CVA capital")).toBeInTheDocument();
		expect(
			screen.getByRole("heading", { name: "Adjoint differentiation" }),
		).toBeInTheDocument();
	});

	it("say why when the portfolio is not covered", async () => {
		server.use(
			http.post("*/api/xva/sensitivities", () =>
				HttpResponse.json(
					{
						code: "unprocessable",
						message: "This portfolio holds a bermudan, valued by regression.",
						detail: null,
					},
					{ status: 422 },
				),
			),
		);
		wrap(<SensitivitiesPanel request={request} />);
		await userEvent.click(
			screen.getByRole("button", { name: "Compute the sensitivities" }),
		);
		// The message, and again in the technical detail under it.
		expect(
			(await screen.findAllByText(/holds a bermudan, valued by regression/))
				.length,
		).toBeGreaterThan(0);
		expect(screen.queryByText("Par swap rates")).not.toBeInTheDocument();
	});
});
