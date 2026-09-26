import type { ComputeDevice } from "@/shared/api";
import type { EngineKey, ProductDescriptor } from "@/shared/products";

/**
 * The request a workbench panel prices -- one function, so that every panel
 * reading the same pricing builds the same body and shares one query.
 * GPU-capable Monte-Carlo gets `device`; off the CPU it also gets Philox,
 * the GPU's generator (blueprint WP 19 §2.3), while a plain CPU run keeps
 * the historical PCG32 stream and its numbers.
 */
export function pricingBody(
	descriptor: ProductDescriptor,
	values: Record<string, unknown>,
	engine: string,
	device: ComputeDevice,
): unknown {
	const base = descriptor.toRequest(values, engine as EngineKey);
	if (!descriptor.gpu || engine !== "mc") return base;
	return {
		...(base as object),
		device,
		...(device === "cpu" ? {} : { rng: "philox" }),
	};
}
