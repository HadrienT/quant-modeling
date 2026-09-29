import { QueryClientProvider } from "@tanstack/react-query";
import { render, screen } from "@testing-library/react";
import { HttpResponse, http } from "msw";
import { beforeEach, describe, expect, it } from "vitest";
import {
	type CreditSpreadsResponse,
	type FundamentalsResponse,
	type StructuralResponse,
	makeQueryClient,
} from "@/shared/api";
import { server } from "@/shared/test/msw-server";
import { TooltipProvider } from "@/shared/ui";
import { CompaniesTab } from "./CompaniesTab";
import { SpreadsTab } from "./SpreadsTab";

/**
 * Credit page against MSW fixtures: a hazard curve is drawn only when it was
 * bootstrapped (otherwise the reason is shown), every statement figure links
 * to its filing, and a company the structural model does not apply to says
 * why instead of showing numbers.
 */

const methodology = [{ title: "Data", paragraphs: ["From FRED."] }];
const company = {
	cik: 1,
	tickers: ["ACME"],
	name: "Acme Corp",
	sic: "3571",
	sic_description: "Electronic Computers",
	last_filed: "2025-08-01",
};

function spreads(hazard: boolean): CreditSpreadsResponse {
	return {
		as_of: "2026-09-28",
		recovery: 0.4,
		discount_curve_as_of: "2026-09-28",
		term_structure: [
			{ series_id: "B1", label: "1-3Y", tenor: 2, spread: 0.0054 },
			{ series_id: "B2", label: "3-5Y", tenor: 4, spread: 0.0072 },
		],
		hazard: hazard
			? {
					times: [2, 4],
					hazards: [0.009, 0.015],
					grid: [0, 2, 4],
					survival: [1, 0.982, 0.953],
					max_repricing_error_bp: 1.6e-11,
				}
			: null,
		hazard_unavailable: hazard
			? null
			: "The hazard curve could not be bootstrapped: arbitrage",
		ratings_as_of: "2026-09-28",
		ratings: [
			{
				series_id: "R1",
				label: "BBB",
				group: "IG",
				spread: 0.0102,
				hazard: 0.017,
				pd_horizons: [1, 5, 10],
				default_probabilities: [0.0168, 0.081, 0.156],
			},
		],
		aggregates: [
			{
				series_id: "BAMLC0A0CM",
				label: "US investment grade",
				as_of: "2026-09-28",
				value: 0.0078,
			},
		],
		reference_yields: [
			{
				series_id: "DBAA",
				label: "Moody's Baa corporate yield",
				as_of: "2026-09-25",
				value: 0.0598,
			},
		],
		methodology,
		warnings: [],
	};
}

const fundamentals: FundamentalsResponse = {
	company,
	frequency: "annual",
	periods: ["2024-12-31"],
	rows: [
		{
			key: "revenue",
			label: "Revenue",
			statement: "income",
			unit: "USD",
			derived: false,
			formula: null,
			values: [
				{
					value: 416_160_000_000,
					sources: [
						{
							concept: "Revenues",
							filed: "2025-02-01",
							accession: "0001-25-000001",
							url: "https://www.sec.gov/Archives/acme-10k.htm",
						},
					],
				},
			],
		},
		{
			key: "gross_profit",
			label: "Gross profit",
			statement: "income",
			unit: "USD",
			derived: false,
			formula: null,
			values: [null],
		},
	],
	ratios: [],
	filings: [],
	methodology,
};

function structural(unavailable: string | null): StructuralResponse {
	return {
		company,
		ticker: "ACME",
		inputs: null,
		unavailable,
		methodology,
		warnings: [],
	};
}

function renderSpreads() {
	return render(
		<QueryClientProvider client={makeQueryClient()}>
			<TooltipProvider>
				<SpreadsTab recovery={0.4} onRecovery={() => {}} />
			</TooltipProvider>
		</QueryClientProvider>,
	);
}

function renderCompanies() {
	return render(
		<QueryClientProvider client={makeQueryClient()}>
			<TooltipProvider>
				<CompaniesTab
					ticker="ACME"
					frequency="annual"
					onTicker={() => {}}
					onFrequency={() => {}}
				/>
			</TooltipProvider>
		</QueryClientProvider>,
	);
}

describe("credit curves", () => {
	it("draws the bootstrapped hazard curve with its repricing check", async () => {
		server.use(
			http.get("*/api/credit/spreads/overview", () =>
				HttpResponse.json(spreads(true)),
			),
			http.get("*/api/credit/spreads/history", () =>
				HttpResponse.json({
					series_id: "BAMLC0A0CM",
					label: "IG",
					kind: "spread",
					points: [],
				}),
			),
		);
		renderSpreads();
		expect(
			await screen.findByText("Cumulative default probability (risk-neutral)"),
		).toBeInTheDocument();
		expect(screen.getByText(/1\.6e-11 bp/)).toBeInTheDocument();
		expect(screen.getByText("102 bp")).toBeInTheDocument(); // BBB OAS
		expect(
			screen.getByRole("heading", { name: "Methodology" }),
		).toBeInTheDocument();
	});

	it("says why when no hazard curve could be bootstrapped", async () => {
		server.use(
			http.get("*/api/credit/spreads/overview", () =>
				HttpResponse.json(spreads(false)),
			),
			http.get("*/api/credit/spreads/history", () =>
				HttpResponse.json({
					series_id: "BAMLC0A0CM",
					label: "IG",
					kind: "spread",
					points: [],
				}),
			),
		);
		renderSpreads();
		expect(
			await screen.findByText(/could not be bootstrapped/),
		).toBeInTheDocument();
		expect(
			screen.queryByText("Cumulative default probability (risk-neutral)"),
		).not.toBeInTheDocument();
	});
});

describe("companies", () => {
	beforeEach(() => {
		server.use(
			http.get("*/api/credit/companies", () =>
				HttpResponse.json({ companies: [company] }),
			),
			http.get("*/api/credit/companies/ACME/fundamentals", () =>
				HttpResponse.json(fundamentals),
			),
			http.get("*/api/credit/spreads/overview", () =>
				HttpResponse.json(spreads(true)),
			),
		);
	});

	it("links every figure to its filing and leaves untagged lines empty", async () => {
		server.use(
			http.get("*/api/credit/companies/ACME/structural", () =>
				HttpResponse.json(structural(null)),
			),
		);
		renderCompanies();
		const revenue = await screen.findByRole("link", { name: "$416.16B" });
		expect(revenue).toHaveAttribute(
			"href",
			"https://www.sec.gov/Archives/acme-10k.htm",
		);
		expect(revenue.getAttribute("title")).toContain("Revenues");
		const gross = screen.getByText("Gross profit").closest("tr")!;
		expect(gross).toHaveTextContent("—");
	});

	it("explains why the structural model is not applied", async () => {
		server.use(
			http.get("*/api/credit/companies/ACME/structural", () =>
				HttpResponse.json(structural("Acme Bank is a financial company.")),
			),
		);
		renderCompanies();
		expect(
			await screen.findByText("Acme Bank is a financial company."),
		).toBeInTheDocument();
		expect(screen.queryByText("Equity value")).not.toBeInTheDocument();
	});
});
