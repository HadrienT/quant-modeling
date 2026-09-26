import { useState } from "react";
import { Gauge } from "lucide-react";
import {
	type ApiError,
	type PricingPath,
	type PricingResult,
	priceOnce,
	useComputeDevices,
} from "@/shared/api";
import { formatDuration, formatNumber } from "@/shared/format";
import { Button, cn } from "@/shared/ui";

/** Below this, kernel launch and copies dominate: the ratio says little. */
const MEANINGFUL_PATHS = 5_000_000;

type Race = { key: string; cpu: PricingResult; gpu: PricingResult };

/**
 * The same Monte-Carlo pricing on one CPU thread and on the GPU (blueprint
 * WP 19 §9). Both runs use Philox with the same seed, hence the same draws:
 * the two prices agree to ~1e-15 and only the time differs. Times are the
 * server's compute times — network excluded — measured one run after the
 * other so they never compete.
 */
export function DeviceRace({
	endpoint,
	body,
	paths,
}: {
	endpoint: PricingPath;
	body: Record<string, unknown>;
	paths: number;
}) {
	const devices = useComputeDevices();
	const [race, setRace] = useState<Race | null>(null);
	const [running, setRunning] = useState(false);
	const [error, setError] = useState<string | null>(null);
	const key = JSON.stringify(body);

	const gpuNames = devices.data?.gpus ?? [];
	if (!gpuNames.length) return null;

	const run = async () => {
		setRunning(true);
		setError(null);
		try {
			const cpu = await priceOnce(endpoint, {
				...body,
				device: "cpu",
				rng: "philox",
			});
			const gpu = await priceOnce(endpoint, {
				...body,
				device: "gpu",
				rng: "philox",
			});
			setRace({ key, cpu, gpu });
		} catch (e) {
			setError((e as ApiError).message ?? String(e));
		} finally {
			setRunning(false);
		}
	};

	const shown = race?.key === key ? race : null;
	const rows = shown
		? [
				{
					label: "CPU",
					sub: `${devices.data?.cpu ?? "CPU"} · 1 thread`,
					r: shown.cpu,
				},
				{ label: "GPU", sub: gpuNames[0]!, r: shown.gpu },
			]
		: [];
	const slowest = Math.max(...rows.map((x) => x.r.compute_ms ?? 0), 1e-9);
	const speedup =
		shown && shown.gpu.compute_ms
			? (shown.cpu.compute_ms ?? 0) / shown.gpu.compute_ms
			: null;

	return (
		<div className="rounded-md border border-hairline bg-surface p-4">
			<div className="flex items-center justify-between gap-3">
				<div className="flex flex-col">
					<span className="text-xs font-semibold text-ink">CPU vs GPU</span>
					<span className="text-2xs text-ink-muted">
						{formatNumber(paths, "plain")} paths, same Philox draws on both
					</span>
				</div>
				<Button size="sm" variant="secondary" disabled={running} onClick={run}>
					<Gauge className="size-3.5" /> {running ? "Racing…" : "Race"}
				</Button>
			</div>

			{error && <p className="mt-2 text-2xs text-critical">{error}</p>}

			{shown && (
				<div className="mt-3 flex flex-col gap-2">
					{rows.map(({ label, sub, r }, i) => (
						<div key={label} className="flex flex-col gap-1">
							<div className="flex items-baseline justify-between gap-2 text-2xs">
								<span className="text-ink-secondary">
									<span className="font-semibold text-ink">{label}</span> ·{" "}
									{sub}
								</span>
								<span className="font-mono text-ink tabular-nums">
									{formatDuration(r.compute_ms)}
								</span>
							</div>
							<div className="h-2 w-full rounded-xs bg-surface-raised">
								<div
									className={cn(
										"h-2 rounded-xs",
										i === 0 ? "bg-series-2" : "bg-series-1",
									)}
									style={{
										width: `${Math.max(0.5, (100 * (r.compute_ms ?? 0)) / slowest)}%`,
									}}
								/>
							</div>
							<span className="font-mono text-2xs text-ink-muted tabular-nums">
								{formatNumber(r.npv, "price")} ±{" "}
								{formatNumber(r.mc_std_error, "price")}
							</span>
						</div>
					))}
					{speedup != null && (
						<p className="text-sm text-ink">
							GPU{" "}
							<span className="font-semibold">
								{speedup.toFixed(speedup < 10 ? 1 : 0)}×
							</span>{" "}
							faster · price gap{" "}
							<span className="font-mono tabular-nums">
								{Math.abs(shown!.cpu.npv - shown!.gpu.npv).toExponential(1)}
							</span>
						</p>
					)}
					{paths < MEANINGFUL_PATHS && (
						<p className="text-2xs text-ink-muted">
							Below ~5 million paths the GPU time is mostly fixed launch cost;
							raise the path count to see the compute ratio.
						</p>
					)}
				</div>
			)}
		</div>
	);
}
