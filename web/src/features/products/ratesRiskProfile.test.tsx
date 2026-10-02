import { QueryClientProvider } from "@tanstack/react-query";
import { render, screen, within } from "@testing-library/react";
import { HttpResponse, http } from "msw";
import { describe, expect, it } from "vitest";
import { makeQueryClient } from "@/shared/api";
import { CATALOG_BY_KEY } from "@/shared/products";
import { server } from "@/shared/test/msw-server";
import rates from "@/shared/test/rates.fixtures.json";
import { RiskProfileTable } from "./RiskProfileTable";

/**
 * The risk profile of a rates product: each row is a reprice on bumped
 * quotes. The fake pricer below is linear in the quotes it receives, so each
 * row's sign and size say exactly which quotes were moved.
 */

type Rows = { rate: number }[];
type Body = {
	curves: { ois: Rows; swaps: Rows };
	swaption_vols?: { normal_vol: number }[];
	swaption?: { strike: number | null };
};

const sum = (rows: Rows) => rows.reduce((n, r) => n + r.rate, 0);

function mount(product: "swap" | "swaption") {
	const bodies: Body[] = [];
	const price = async ({ request }: { request: Request }) => {
		const body = (await request.json()) as Body;
		bodies.push(body);
		// Long the index quotes, short the OIS quotes twice over, long vol.
		const npv =
			1e6 * (sum(body.curves.swaps) - 2 * sum(body.curves.ois)) +
			1e9 * (body.swaption_vols?.[0]?.normal_vol ?? 0);
		return HttpResponse.json({ ...rates.swaption.european, npv });
	};
	server.use(
		http.post("*/price/rates/swap", price),
		http.post("*/price/rates/swaption", price),
	);
	render(
		<QueryClientProvider client={makeQueryClient()}>
			<RiskProfileTable descriptor={CATALOG_BY_KEY.get(product)!} />
		</QueryClientProvider>,
	);
	return bodies;
}

const position = (factor: string) =>
	within(screen.getByRole("row", { name: new RegExp(`^${factor}`) })).getByText(
		/^(long|short|negligible|not significant)$/,
	).textContent;

describe("rates risk profile", () => {
	it("bumps the whole curve, then the index quotes alone", async () => {
		mount("swap");
		await screen.findByText("Index basis over OIS");
		// +1 bp everywhere: the OIS quotes weigh twice the index ones.
		expect(position("Interest rates \\(parallel\\)")).toBe("short");
		expect(position("Index basis over OIS")).toBe("long");
		expect(screen.queryByText("Volatility (vega)")).not.toBeInTheDocument();
		expect(screen.getByText(/illustrative EUR quotes/)).toBeInTheDocument();
	});

	it("strikes the swaption at today's forward before bumping, and adds vega", async () => {
		const bodies = mount("swaption");
		await screen.findByText("Volatility (vega)");
		expect(position("Volatility \\(vega\\)")).toBe("long");
		// The first pricing finds the forward; every bumped one is struck at it.
		expect(bodies[0]!.swaption!.strike).toBeNull();
		const forward = rates.swaption.european.swaption.forward;
		expect(bodies.length).toBe(5);
		for (const b of bodies.slice(1))
			expect(b.swaption!.strike).toBeCloseTo(forward, 12);
	});
});
