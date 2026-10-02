import { QueryClientProvider } from "@tanstack/react-query";
import {
	fireEvent,
	render,
	screen,
	waitFor,
	within,
} from "@testing-library/react";
import { HttpResponse, http } from "msw";
import { describe, expect, it } from "vitest";
import { makeQueryClient } from "@/shared/api";
import { pricingResponse } from "@/shared/test/fixtures";
import { server } from "@/shared/test/msw-server";
import { TooltipProvider } from "@/shared/ui";
import ScriptingPreview from "./ScriptingPreview";

/**
 * The scripting page: a price comes with its greeks (asked for by default,
 * each with its standard error, noise marked as such), and the assistant
 * says whether its model server is up before anyone types to it.
 */

const RISKS = [
	{ label: "spot", value: 0.5987, std_error: 0.0001 },
	{ label: "vol", value: 38.66, std_error: 0.02 },
	// Within two standard errors of zero: noise.
	{ label: "kappa", value: 3.1, std_error: 2.4 },
	// Exactly zero: a parameter that does not enter the price.
	{ label: "jump_vol", value: 0, std_error: 0 },
	{ label: "lvol[0,0]", value: 0.01, std_error: 0.001 },
	{ label: "lvol[0,1]", value: 0.02, std_error: 0.001 },
];

function mount(status?: Record<string, unknown>) {
	const bodies: { greeks_method?: string }[] = [];
	server.use(
		http.post("*/price/scripted", async ({ request }) => {
			const body = (await request.json()) as { greeks_method?: string };
			bodies.push(body);
			return HttpResponse.json(
				pricingResponse({
					risks: body.greeks_method === "aad" ? RISKS : null,
					warnings: [],
				}),
			);
		}),
	);
	if (status)
		server.use(
			http.get("*/api/assistant/status", () =>
				HttpResponse.json({
					model: "Qwen3-Coder-30B-A3B-Instruct",
					serving: [],
					checked_at: "2026-10-02T10:00:00Z",
					...status,
				}),
			),
		);
	render(
		<QueryClientProvider client={makeQueryClient()}>
			<TooltipProvider>
				<ScriptingPreview />
			</TooltipProvider>
		</QueryClientProvider>,
	);
	return bodies;
}

const price = () =>
	fireEvent.click(screen.getByRole("button", { name: "Price" }));

describe("scripting page — greeks", () => {
	it("asks for the greeks by default and shows them with their errors", async () => {
		const bodies = mount();
		price();
		const greeks = await screen.findByRole("region", { name: "Greeks" });
		expect(bodies[0]!.greeks_method).toBe("aad");
		expect(
			within(greeks).getByRole("row", { name: /^Delta/ }),
		).toHaveTextContent("0.5987");
		expect(
			within(greeks).getByRole("row", { name: /^Vega/ }),
		).toHaveTextContent("±");
		// Noise is called noise; an exact zero is named, not tabulated.
		expect(
			within(greeks).getByRole("row", { name: /Heston κ/ }),
		).toHaveTextContent("not significant");
		expect(
			within(greeks).queryByRole("row", { name: /jump_vol/ }),
		).not.toBeInTheDocument();
		expect(within(greeks).getByText(/Exactly zero/)).toHaveTextContent(
			"jump_vol",
		);
		expect(
			within(greeks).getByText(/Plus 2 sensitivities to the nodes/),
		).toBeInTheDocument();
	});

	it("prices without greeks when they are switched off", async () => {
		const bodies = mount();
		fireEvent.click(screen.getByLabelText("Greeks (adjoint)"));
		price();
		await waitFor(() => expect(bodies).toHaveLength(1));
		expect(bodies[0]!.greeks_method).toBe("none");
		await screen.findByText(/Present value/);
		expect(
			screen.queryByRole("region", { name: "Greeks" }),
		).not.toBeInTheDocument();
	});
});

describe("scripting page — assistant status", () => {
	it("says the assistant is online", async () => {
		mount();
		expect(
			await screen.findByRole("status", { name: "Assistant status: Online" }),
		).toBeInTheDocument();
		expect(screen.queryByRole("alert")).not.toBeInTheDocument();
	});

	it("says the model server is down, in words and not only in colour", async () => {
		mount({
			state: "down",
			detail: "The model server does not answer: the assistant is offline.",
		});
		expect(
			await screen.findByRole("status", { name: "Assistant status: Offline" }),
		).toBeInTheDocument();
		expect(screen.getByRole("alert")).toHaveTextContent("does not answer");
	});

	it("names the other model when the server was switched to one", async () => {
		mount({
			state: "wrong_model",
			serving: ["plamo-2-translate"],
			detail:
				"The model server is running plamo-2-translate, not the assistant's model (Qwen3-Coder-30B-A3B-Instruct): answers would come from that model.",
		});
		expect(
			await screen.findByRole("status", {
				name: "Assistant status: Other model loaded",
			}),
		).toBeInTheDocument();
		expect(screen.getByRole("alert")).toHaveTextContent("plamo-2-translate");
	});
});
