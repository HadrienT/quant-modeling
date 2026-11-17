import { useState } from "react";
import { useSimulationDistribution } from "@/shared/api";
import type { DistributionRequest } from "@/shared/api/types";

export type StochasticModel = "heston" | "slv";

/**
 * Local vol against a stochastic-vol model on one ticker (WP 29, lot S4):
 * two distribution requests on the same stored surface, grid, paths and
 * seed. Local vol reprices the surface by construction (Dupire), SLV too
 * (its leverage), Heston as well as five parameters allow; their dynamics
 * differ, which the forward smile and the barrier price show.
 */
export function useComparison() {
	const [other, setOther] = useState<StochasticModel>("slv");
	const localVol = useSimulationDistribution();
	const stochastic = useSimulationDistribution();

	function run(base: Omit<DistributionRequest, "model">) {
		const req = { ...base, n_display: 0 };
		localVol.mutate({ ...req, model: "local_vol" });
		stochastic.mutate({ ...req, model: other });
	}

	return {
		other,
		setOther,
		localVol,
		stochastic,
		run,
		isPending: localVol.isPending || stochastic.isPending,
		error: localVol.error ?? stochastic.error,
	};
}

export type UseComparison = ReturnType<typeof useComparison>;
