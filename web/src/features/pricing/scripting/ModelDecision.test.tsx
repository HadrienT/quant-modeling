import { render, screen } from "@testing-library/react";
import { describe, expect, it } from "vitest";
import type { ModelChoice } from "@/shared/api";
import { ModelDecision } from "./ModelDecision";

const slv: ModelChoice = {
	requested: "auto",
	model: "slv",
	code: "path_dependent",
	reason: "The payoff carries state across dates.",
	calibration: {
		ticker: "SPY",
		snapshot: "2026-09-24",
		seconds: 3.7,
		heston: {
			v0: 0.0179,
			kappa: 0.0592,
			theta: 0.4675,
			xi: 0.6307,
			rho: -0.6255,
			iv_rmse: 0.0182,
			iv_worst: 0.0774,
			n_quotes: 160,
			n_maturities: 20,
			feller: false,
		},
		leverage: { min: 0.06, max: 5, clamped_share: 0.0016, n_particles: 50000 },
	},
};

describe("ModelDecision", () => {
	it("announces an automatic choice with its reason", () => {
		render(<ModelDecision choice={slv} />);
		expect(
			screen.getByText("Stochastic-local vol (calibrated)"),
		).toBeInTheDocument();
		expect(screen.getByText("chosen automatically")).toBeInTheDocument();
		expect(
			screen.getByText("The payoff carries state across dates."),
		).toBeInTheDocument();
	});

	it("shows the calibrated parameters and the fit error in vol points", () => {
		render(<ModelDecision choice={slv} />);
		expect(screen.getByText(/RMSE 1\.82%, worst 7\.74%/)).toBeInTheDocument();
		expect(screen.getByText(/13\.38% vol/)).toBeInTheDocument(); // sqrt(v0)
		expect(screen.getByText(/does not hold/)).toBeInTheDocument();
		expect(screen.getByText(/0\.16% of the grid/)).toBeInTheDocument();
	});

	it("says a hand-picked model was chosen by hand, without a reason", () => {
		render(
			<ModelDecision
				choice={{
					requested: "black_scholes",
					model: "black_scholes",
					code: "user",
					reason: "Chosen by hand.",
				}}
			/>,
		);
		expect(screen.getByText("chosen by hand")).toBeInTheDocument();
		expect(screen.queryByText("Chosen by hand.")).not.toBeInTheDocument();
	});
});
