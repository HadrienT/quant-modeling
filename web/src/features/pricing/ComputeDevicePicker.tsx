import { Cpu, Zap } from "lucide-react";
import { type ComputeDevice, useComputeDevices } from "@/shared/api";
import { cn } from "@/shared/ui";

const CHOICES: { key: ComputeDevice; label: string }[] = [
	{ key: "cpu", label: "CPU" },
	{ key: "gpu", label: "GPU" },
	{ key: "auto", label: "Auto" },
];

/**
 * Where the Monte-Carlo paths run (blueprint WP 19 §8). GPU is greyed, with
 * the reason, when the server has no usable CUDA device.
 */
export function ComputeDevicePicker({
	device,
	onChange,
}: {
	device: ComputeDevice;
	onChange: (d: ComputeDevice) => void;
}) {
	const q = useComputeDevices();
	const gpus = q.data?.gpus ?? [];
	const noGpu = !q.isLoading && gpus.length === 0;
	const why = q.data?.gpu_compiled
		? "The server sees no CUDA device right now."
		: "This server's pricing library was built without the CUDA backend.";

	return (
		<div className="flex flex-col gap-1">
			<span className="text-2xs font-semibold tracking-wide text-ink-muted uppercase">
				Compute · Monte-Carlo
			</span>
			<div className="flex flex-wrap items-center gap-1">
				{CHOICES.map((c) => {
					const disabled = c.key === "gpu" && noGpu;
					return (
						<button
							key={c.key}
							type="button"
							disabled={disabled}
							onClick={() => onChange(c.key)}
							title={
								disabled
									? why
									: c.key === "auto"
										? "GPU when the server has one, CPU otherwise"
										: c.key === "gpu"
											? gpus[0]
											: `${q.data?.cpu ?? "CPU"} — one thread`
							}
							className={cn(
								"inline-flex items-center gap-1 rounded-sm border px-2 py-1 text-xs",
								device === c.key
									? "border-accent bg-accent/10 text-ink"
									: "border-hairline text-ink-secondary hover:text-ink",
								disabled &&
									"cursor-not-allowed opacity-50 hover:text-ink-secondary",
							)}
						>
							{c.key === "cpu" && <Cpu className="size-3.5" />}
							{c.key === "gpu" && <Zap className="size-3.5" />}
							{c.label}
						</button>
					);
				})}
				<span className="text-2xs text-ink-muted">
					{gpus.length > 0
						? `${gpus.length} × ${gpus[0]}`
						: noGpu
							? "no GPU on this server"
							: ""}
				</span>
			</div>
		</div>
	);
}
