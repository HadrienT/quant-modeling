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
