import { render, screen } from "@testing-library/react";
import { describe, expect, it } from "vitest";
import { ScriptGreeks } from "./ScriptGreeks";

describe("ScriptGreeks on a rates product", () => {
	it("names the curve's quotes and the short rate's vol", () => {
		render(
			<ScriptGreeks
				risks={[
					{ label: "rate_vol", value: 1.54e7, std_error: 1.2e3 },
					{ label: "swap_rate[6]", value: 2.47e7, std_error: 4e3 },
					{ label: "zero[0.5]", value: -3.1e5, std_error: 50 },
					{ label: "swap_rate[30]", value: 0, std_error: 0 },
					{ label: "curve_parallel", value: 2.07e7, std_error: 4.4e3 },
				]}
			/>,
		);
		expect(screen.getByText("Hull-White σ")).toBeInTheDocument();
		expect(screen.getByText("SOFR 6Y par swap rate")).toBeInTheDocument();
		expect(screen.getByText("Zero rate at 0.5Y")).toBeInTheDocument();
		expect(screen.getByText("Curve, every quote together")).toBeInTheDocument();
		// an exactly-zero risk is named apart, not tabulated
		expect(screen.queryByRole("cell", { name: /SOFR 30Y/ })).toBeNull();
	});
});

describe("ScriptGreeks on the market's forward curve", () => {
	it("splits Rho and the dividend risk by expiry, apart from the greeks", () => {
		render(
			<ScriptGreeks
				risks={[
					{ label: "spot", value: 0.6, std_error: 0.01 },
					{ label: "rate", value: -40, std_error: 0.5 },
					{ label: "div", value: -25, std_error: 0.4 },
					{ label: "zero[0.005479452055]", value: 0, std_error: 0 },
					{ label: "zero[0.25]", value: -10, std_error: 0.1 },
					{ label: "div_yield[0.25]", value: -25, std_error: 0.4 },
					{ label: "zero[2]", value: -30, std_error: 0.4 },
					{ label: "div_yield[2]", value: 0, std_error: 0 },
				]}
			/>,
		);
		expect(screen.getByText("Rho")).toBeInTheDocument();
		expect(
			screen.getByText(/Rho and dividend by expiry · 2 of 3 expiries/),
		).toBeInTheDocument();
		expect(screen.getByText("0.25 y")).toBeInTheDocument();
		expect(screen.getByText("2.00 y")).toBeInTheDocument();
		// the pillars are not greeks rows, nor named as exactly zero
		expect(screen.queryByText(/Zero rate at/)).toBeNull();
		expect(screen.queryByText(/Exactly zero/)).toBeNull();
	});
});
