import { useState } from "react";
import type { HestonParams } from "@/shared/api/types";

/** Heston's five parameters as the form shows them: v0 and theta as the
 * volatilities they stand for (in %), the others as they are. */
export function useHestonParams() {
	const [v0Pct, setV0Pct] = useState("20");
	const [kappa, setKappa] = useState("2");
	const [thetaPct, setThetaPct] = useState("20");
	const [xi, setXi] = useState("0.5");
	const [rho, setRho] = useState("-0.7");

	const toParams = (): HestonParams => ({
		v0: (Number(v0Pct) / 100) ** 2,
		kappa: Number(kappa),
		theta: (Number(thetaPct) / 100) ** 2,
		xi: Number(xi),
		rho: Number(rho),
	});
	const fromFit = (h: HestonParams) => {
		setV0Pct((Math.sqrt(h.v0) * 100).toFixed(2));
		setKappa(h.kappa.toFixed(4));
		setThetaPct((Math.sqrt(h.theta) * 100).toFixed(2));
		setXi(h.xi.toFixed(4));
		setRho(h.rho.toFixed(4));
	};

	return {
		fields: [
			{ label: "Initial vol % (sqrt v0)", value: v0Pct, set: setV0Pct },
			{ label: "Mean reversion (kappa)", value: kappa, set: setKappa },
			{
				label: "Long-run vol % (sqrt theta)",
				value: thetaPct,
				set: setThetaPct,
			},
			{ label: "Vol of vol (xi)", value: xi, set: setXi },
			{ label: "Spot / vol correlation (rho)", value: rho, set: setRho },
		],
		toParams,
		fromFit,
	};
}

export type UseHestonParams = ReturnType<typeof useHestonParams>;
