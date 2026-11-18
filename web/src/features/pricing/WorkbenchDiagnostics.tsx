import { useMemo, useState } from "react";
import { ChevronRight } from "lucide-react";
import {
	type ComputeDevice,
	useComputeDevices,
	usePricing,
} from "@/shared/api";
import type { ProductDescriptor } from "@/shared/products";
import { Button, cn } from "@/shared/ui";
import { ConvergencePanel } from "./ConvergencePanel";
import { DeviceRace } from "./DeviceRace";
import { MarketVegaPanel } from "./MarketVegaPanel";
import { ModelComparison } from "./ModelComparison";
import { RatesScriptDiagnostics } from "./rates/RatesScriptDiagnostics";
import { pricingBody } from "./requestBody";

/**
 * What a quant opens to check a price, not what a visitor reads first
 * (WP 28, lot U3): Monte Carlo convergence, the CPU / GPU race, the model
 * comparison and the vega to market quotes. Closed by default, its state in
 * the URL (`?diag=true`) so a shared link reopens it. Nothing here is priced
 * while it is closed: each panel mounts, and asks the API, only once open.
 */
export function WorkbenchDiagnostics({
	descriptor,
	values,
	engine,
	device,
	open,
	onOpenChange,
}: {
	descriptor: ProductDescriptor;
	values: Record<string, unknown>;
	engine: string;
	device: ComputeDevice;
	open: boolean;
	onOpenChange: (open: boolean) => void;
}) {
	const devices = useComputeDevices();
	const isMc = engine === "mc";
	// Model comparison and market vega reprice the script by Monte-Carlo: not
	// offered next to its closed form.
	// Typed inputs have no market to compare models on or take a vega to.
	const typed = values.source === "typed";
	const script = isMc && !typed ? descriptor.scripted : undefined;
	// The race needs a GPU on the server, not only a product that runs on one.
	const race =
		descriptor.gpu &&
		descriptor.endpoint &&
		(devices.data?.gpus ?? []).length > 0;
	// A swap or swaption script: the rates engines on the same trade, under
	// either engine.
	const sofr = descriptor.scripted?.curve === "sofr" && !typed;
	const offered = isMc || race || script || sofr;
	if (!offered) return null;
	const what = [
		sofr && "curve, Hull-White calibration, cash flows, market models",
		isMc && "convergence",
		race && "CPU / GPU race",
		script &&
			script.underlyings === 1 &&
			!script.market &&
			"model comparison, market vega",
	]
		.filter(Boolean)
		.join(" · ");
	return (
		<section className="rounded-md border border-hairline bg-surface">
			<button
				type="button"
				aria-expanded={open}
				onClick={() => onOpenChange(!open)}
				className="flex w-full items-center gap-2 px-4 py-3 text-left text-sm"
			>
				<ChevronRight
					className={cn(
						"size-4 text-ink-muted transition-transform",
						open && "rotate-90",
					)}
					aria-hidden="true"
				/>
				<span className="font-medium text-ink">Diagnostics</span>
				<span className="truncate text-xs text-ink-muted">{what}</span>
			</button>
			{open && (
				<div className="flex flex-col gap-4 border-t border-hairline p-4">
					<Panels
						descriptor={descriptor}
						values={values}
						engine={engine}
						device={device}
					/>
				</div>
			)}
		</section>
	);
}

function Panels({
	descriptor,
	values,
	engine,
	device,
}: {
	descriptor: ProductDescriptor;
	values: Record<string, unknown>;
	engine: string;
	device: ComputeDevice;
}) {
	const [compare, setCompare] = useState(false);
	const body = useMemo(
		() => pricingBody(descriptor, values, engine, device),
		[descriptor, values, engine, device],
	);
	// The same query as the results above: read from the cache, not priced again.
	const q = usePricing({ endpoint: descriptor.endpoint ?? null, body });
	const script = values.source === "typed" ? undefined : descriptor.scripted;
	return (
		<>
			{script?.curve === "sofr" && (
				<RatesScriptDiagnostics product={script} values={values} />
			)}
			{engine === "mc" && q.data && (
				<ConvergencePanel
					endpoint={descriptor.endpoint ?? null}
					body={body}
					priced={q.data}
				/>
			)}
			{descriptor.gpu && descriptor.endpoint && (
				<DeviceRace
					endpoint={descriptor.endpoint}
					body={descriptor.toRequest(values, "mc") as Record<string, unknown>}
					paths={Number(values.n_paths) || 0}
				/>
			)}
			{script &&
				!script.market &&
				(script.underlyings === 1 ? (
					<div className="flex flex-col gap-2">
						<Button
							size="sm"
							variant="secondary"
							className="w-fit"
							aria-pressed={compare}
							onClick={() => setCompare((c) => !c)}
						>
							{compare ? "Hide the model comparison" : "Compare the dynamics"}
						</Button>
						{compare && <ModelComparison product={script} values={values} />}
						<MarketVegaPanel body={body} />
					</div>
				) : (
					<p className="text-2xs text-ink-muted">
						Several underlyings: correlated Black-Scholes is the only
						multi-asset model for scripts, so there is nothing to compare yet.
					</p>
				))}
		</>
	);
}
