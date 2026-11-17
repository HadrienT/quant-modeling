import type { DistributionResponse } from "@/shared/api/types";
import { FanChart, useChartTheme } from "@/shared/viz";
import { quantileBands } from "./distribution";

const WHAT: Record<string, string> = {
	black_scholes: "sigma, the same on every path",
	sabr: "alpha_t F_t^(beta-1), the lognormal-equivalent vol",
	local_vol: "sigma_loc(S_t, t), read on each path's spot",
	heston: "sqrt(v_t), the variance process",
	slv: "L(S_t, t) sqrt(v_t), the leverage times the Heston vol",
};

/**
 * The volatility the paths diffuse with (WP 29): quantile bands of the
 * instantaneous vol sigma(t) of d ln S over the paths, a few paths, and the
 * model's ATM implied vol term structure on the same axis, with its 95%
 * Monte Carlo band. The two are different objects: sigma(t) is the vol at
 * date t on a path, the implied vol at t averages the vol up to t.
 */
export function VolPanel({
	data,
	revealCount,
	cursor,
	setCursor,
}: {
	data: DistributionResponse;
	revealCount?: number;
	cursor: number;
	setCursor: (i: number) => void;
}) {
	const t = useChartTheme();
	if (!data.vol_quantiles) return null;
	const { bands, median } = quantileBands(
		data.quantile_levels,
		data.vol_quantiles,
	);
	const atm = data.atm_vol;
	const se = data.atm_vol_stderr;
	return (
		<FanChart
			title="Instantaneous volatility"
			subtitle={`sigma(t) of d ln S: ${WHAT[data.model] ?? ""}`}
			time={data.time_grid}
			bands={[
				...bands,
				{
					label: "ATM implied vol, 95% band",
					lo: atm.map((v, i) => (v == null ? null : v - 1.96 * (se[i] ?? 0))),
					hi: atm.map((v, i) => (v == null ? null : v + 1.96 * (se[i] ?? 0))),
				},
			]}
			paths={(data.vols ?? []).slice(0, 8)}
			lines={[
				...(median ? [{ label: "Median sigma(t)", y: median }] : []),
				{
					label: "ATM implied vol of maturity t",
					y: atm,
					color: t.series[2],
					dashed: true,
				},
			]}
			revealCount={revealCount}
			cursor={cursor}
			onCursor={setCursor}
			yFormat={(v) => `${(v * 100).toFixed(0)}%`}
			height={260}
		/>
	);
}
