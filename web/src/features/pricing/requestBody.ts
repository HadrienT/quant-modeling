import type { ComputeDevice } from "@/shared/api";
import type { EngineKey, ProductDescriptor } from "@/shared/products";

/**
 * The request a workbench panel prices -- one function, so that every panel
 * reading the same pricing builds the same body and shares one query.
 * GPU-capable Monte-Carlo gets `device`; off the CPU it also gets Philox,
 * the GPU's generator (blueprint WP 19 §2.3), while a plain CPU run keeps
 * the historical PCG32 stream and its numbers. A product of the script
 * library asks for its greeks by adjoint (AAD), as the scripting page does:
 * the API computes none unless asked (`greeks_method`, "none" by default).
 */
export function pricingBody(
	descriptor: ProductDescriptor,
	values: Record<string, unknown>,
	engine: string,
	device: ComputeDevice,
): unknown {
	const request = descriptor.toRequest(values, engine as EngineKey);
	const base = descriptor.scripted
		? { ...(request as object), greeks_method: "aad" }
		: request;
	if (!descriptor.gpu || engine !== "mc") return base;
	return {
		...(base as object),
		device,
		...(device === "cpu" ? {} : { rng: "philox" }),
	};
}
