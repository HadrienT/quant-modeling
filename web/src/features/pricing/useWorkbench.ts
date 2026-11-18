import { useCallback, useMemo } from "react";
import { useNavigate, useSearch } from "@tanstack/react-router";
import { decodeParams, encodeParams } from "@/shared/products/workbenchLink";
import type { ComputeDevice } from "@/shared/api";
import {
	CATALOG_BY_KEY,
	DEFAULT_PRODUCT_KEY,
	resolveProductKey,
	type EngineKey,
} from "@/shared/products";

/**
 * All view state lives in the URL (WP 07 §6): a pricing session is a link.
 * `p` is a base64url-encoded JSON of the form values. Kept under ~120 lines.
 */

export type WorkbenchState = {
	productKey: string;
	engine: EngineKey;
	values: Record<string, unknown>;
	compare: Record<string, unknown> | null;
};

const encode = encodeParams;
const decode = decodeParams;

export function useWorkbench() {
	const search = useSearch({ from: "/price" });
	const navigate = useNavigate({ from: "/price" });

	// A former key (a product a script now prices) opens that script.
	const asked = resolveProductKey(search.product);
	const productKey =
		asked && CATALOG_BY_KEY.has(asked) ? asked : DEFAULT_PRODUCT_KEY;
	const descriptor = CATALOG_BY_KEY.get(productKey)!;

	// An engine the product does not offer (an old link to a tree or the PDE,
	// gone from the site in issue 313) opens its first one.
	const engine = (
		descriptor.engines.some((e) => e.key === search.engine)
			? search.engine
			: descriptor.engines[0]!.key
	) as EngineKey;
	const values = useMemo(
		() => ({ ...descriptor.defaults, ...decode(search.p, {}) }),
		[descriptor, search.p],
	);
	const compare = decode<Record<string, unknown> | null>(search.compare, null);
	const device: ComputeDevice = search.device ?? "cpu";
	const diagnostics = search.diag === true;

	const setProduct = useCallback(
		(key: string) => {
			navigate({
				search: (prev) => ({
					product: key,
					engine: undefined,
					p: undefined,
					diag: prev.diag,
				}),
			});
		},
		[navigate],
	);

	const setEngine = useCallback(
		(e: EngineKey) => {
			navigate({ search: (prev) => ({ ...prev, engine: e }) });
		},
		[navigate],
	);

	const setValues = useCallback(
		(next: Record<string, unknown>) => {
			navigate({ search: (prev) => ({ ...prev, p: encode(next) }) });
		},
		[navigate],
	);

	// Only Monte-Carlo runs on the GPU: asking for it switches the engine.
	const setDevice = useCallback(
		(d: ComputeDevice) => {
			navigate({
				search: (prev) => ({
					...prev,
					device: d === "cpu" ? undefined : d,
					engine: d === "cpu" ? prev.engine : "mc",
				}),
			});
		},
		[navigate],
	);

	const setCompare = useCallback(
		(next: Record<string, unknown> | null) => {
			navigate({
				search: (prev) => ({
					...prev,
					compare: next ? encode(next) : undefined,
				}),
			});
		},
		[navigate],
	);

	const setDiagnostics = useCallback(
		(open: boolean) => {
			navigate({
				search: (prev) => ({ ...prev, diag: open || undefined }),
				replace: true,
			});
		},
		[navigate],
	);

	return {
		descriptor,
		productKey,
		engine,
		values,
		compare,
		device,
		setDevice,
		setProduct,
		setEngine,
		setValues,
		setCompare,
		diagnostics,
		setDiagnostics,
	};
}
